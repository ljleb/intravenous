#include <intravenous/graph_jit/lowering/event_port_plan.h>

#include <intravenous/graph_jit/background_storage_plan.h>
#include <intravenous/graph_jit/lowering.h>
#include <intravenous/graph_jit/lowering/graph_analysis.h>
#include <intravenous/graph_jit/transient_arena_plan.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iv::graph_jit::detail {
namespace {

struct EventFeedbackPlanningDecision {
    EventConnectionStorageRequirements requirements{};
    EventConnectionStoragePlan storage{};
};

struct EventPlanningDecisions {
    std::vector<std::optional<EventConnectionStoragePlan>> producer_groups{};
    std::vector<EventFeedbackPlanningDecision> feedback{};
};

std::expected<EventPlanningDecisions, std::string> select_event_storage(
    EventPortBindingPlan const& plan,
    std::vector<EventConnectionStorageRequirements> group_requirements,
    RealtimeStorageCostModel const& cost_model)
{
    EventPlanningDecisions selected;
    selected.producer_groups.resize(group_requirements.size());
    selected.feedback.reserve(plan.feedback_operations.size());

    // A raw output has no producer group. Charge the group that owns the
    // derived sequence when the operation reads a raw output instead.
    auto producer_group = [&](std::size_t source, std::size_t target)
        -> std::optional<std::size_t> {
        if (source >= plan.ports.size()) return std::nullopt;
        auto group = plan.ports[source].producer_group_index;
        if (group >= group_requirements.size() && target < plan.ports.size())
            group = plan.ports[target].producer_group_index;
        if (group >= group_requirements.size()) return std::nullopt;
        return group;
    };

    // Count each shared feedback stream and conversion exactly once.
    std::vector<std::optional<std::size_t>> feedback_by_target;
    if (!plan.feedback_operations.empty()) feedback_by_target.resize(plan.ports.size());
    for (auto const& feedback : plan.feedback_operations) {
        auto const group = producer_group(feedback.source_representation, feedback.target_representation);
        if (!group || feedback.target_representation >= feedback_by_target.size())
            return std::unexpected("GraphJit event costing lost a feedback producer group");
        feedback_by_target[feedback.target_representation] = selected.feedback.size();
        selected.feedback.push_back({.requirements = feedback.storage_requirements});
        auto& ring_reads = group_requirements[*group].operations.full_ring_addressed_values;
        ring_reads = iv::detail::saturating_add(ring_reads, feedback.source_event_reads);
    }

    for (auto const& operation : plan.materializations) {
        if (operation.source_representation >= plan.ports.size())
            return std::unexpected("GraphJit event costing found an invalid materialization source");
        auto const feedback = feedback_by_target.empty() ? std::nullopt : feedback_by_target[operation.source_representation];
        RealtimeStorageOperationCounts* costs;
        if (feedback) {
            costs = &selected.feedback[*feedback].requirements.operations;
        } else {
            auto const group = producer_group(operation.source_representation, operation.target_representation);
            if (!group) return std::unexpected("GraphJit event costing lost a materialization producer group");
            costs = &group_requirements[*group].operations;
        }
        costs->invariant_copied_values = iv::detail::saturating_add(
            costs->invariant_copied_values, operation.maximum_output_event_count);
        costs->full_ring_addressed_values = iv::detail::saturating_add(
            costs->full_ring_addressed_values, operation.maximum_source_event_reads);
    }

    for (std::size_t index = 0; index < group_requirements.size(); ++index) {
        if (!plan.producer_group_representations[index]) continue;
        auto storage = choose_event_connection_storage_plan(group_requirements[index], cost_model);
        if (!storage.candidate_costs.for_kind(storage.kind).legal)
            return std::unexpected(
                "GraphJit event operations have no storage realization within the compile-time stack budget");
        selected.producer_groups[index] = storage;
    }
    for (auto& feedback : selected.feedback) {
        feedback.storage = choose_event_connection_storage_plan(feedback.requirements, cost_model);
        if (!feedback.storage.candidate_costs.for_kind(feedback.storage.kind).legal)
            return std::unexpected(
                "GraphJit event feedback has no storage realization within the compile-time stack budget");
    }
    return selected;
}

std::expected<void, std::string> layout_event_sequence(EventRepresentationPlan& port)
{
    auto const capacity = port.event_capacity;
    auto const persistent = port.persistent;
    auto const persistent_ring = port.persistent_ring;
    auto const source_indices = port.has_source_indices;
    auto align_up = [](std::size_t value, std::size_t alignment)
        -> std::optional<std::size_t> {
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
            return std::nullopt;
        }
        auto const mask = alignment - 1;
        if (value > std::numeric_limits<std::size_t>::max() - mask) {
            return std::nullopt;
        }
        return (value + mask) & ~mask;
    };
    if (persistent_ring && !persistent) {
        return std::unexpected(
            "GraphJit persistent event ring must use persistent storage");
    }
    std::size_t const read_index_relative = 0;
    std::size_t const write_index_relative = persistent_ring
        ? sizeof(std::size_t)
        : 0;
    std::size_t header_end = persistent_ring
        ? 2 * sizeof(std::size_t)
        : sizeof(std::size_t);
    std::size_t alignment = std::max(
        alignof(std::size_t), alignof(TimedEvent));
    std::size_t timeline_initialized_relative = 0;
    std::size_t expected_next_time_relative = 0;
    if (persistent) {
        timeline_initialized_relative = header_end;
        auto const expected_relative = align_up(
            header_end + sizeof(std::uint8_t), alignof(GlobalIndex));
        if (!expected_relative
            || *expected_relative
                > std::numeric_limits<std::size_t>::max()
                    - sizeof(GlobalIndex)) {
            return std::unexpected(
                "GraphJit event timeline header size overflows size_t");
        }
        expected_next_time_relative = *expected_relative;
        header_end = expected_next_time_relative + sizeof(GlobalIndex);
        alignment = std::max(alignment, alignof(GlobalIndex));
    }
    auto const events_relative = align_up(header_end, alignof(TimedEvent));
    if (!events_relative
        || capacity > (std::numeric_limits<std::size_t>::max()
                - *events_relative) / sizeof(TimedEvent)) {
        return std::unexpected(
            "GraphJit event storage size overflows size_t");
    }
    auto const events_end = *events_relative + capacity * sizeof(TimedEvent);
    std::size_t source_indices_relative = 0;
    auto size_bytes = events_end;
    if (source_indices) {
        auto const indices_relative = align_up(
            events_end, alignof(std::size_t));
        if (!indices_relative
            || capacity > (std::numeric_limits<std::size_t>::max()
                    - *indices_relative) / sizeof(std::size_t)) {
            return std::unexpected(
                "GraphJit event source-index storage size overflows size_t");
        }
        source_indices_relative = *indices_relative;
        size_bytes = source_indices_relative
            + capacity * sizeof(std::size_t);
        alignment = std::max(alignment, alignof(std::size_t));
    }
    port.read_index_relative_offset = read_index_relative;
    port.write_index_relative_offset = write_index_relative;
    port.has_sequential_timeline = persistent;
    port.timeline_initialized_relative_offset = timeline_initialized_relative;
    port.expected_next_time_relative_offset = expected_next_time_relative;
    port.events_relative_offset = *events_relative;
    port.source_indices_relative_offset = source_indices_relative;
    port.size_bytes = size_bytes;
    port.alignment = alignment;
    return {};
}

struct EventPortInventory {
    std::vector<std::optional<std::size_t>> primitives;
    std::vector<PlannedGraphNode const*> nodes;
};

// Group and feedback buffers have the same working capacity in every legal
// storage kind: transient storage is legal only when there are no retained
// events. Consumers can size their sequences before choosing where to store
// those events.
struct EventSequenceStoragePlan {
    std::size_t representation = 0;
    std::size_t retained_event_count = 0;
    EventOperationScope begin{};
    EventOperationScope end{};
    std::size_t history = 0;
    std::size_t latency = 0;
    std::string identity_base{};
    bool ordered_fan_in = false;
    bool feedback = false;
    bool reset_transient = false;
    std::optional<std::size_t> transient_alias{};
    std::optional<std::size_t> append_merge{};
};

std::expected<std::size_t, std::string> rounded_event_capacity(std::size_t required)
{
    if (required == 0) return std::size_t{0};
    constexpr auto highest_power_of_two =
        std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    if (required > highest_power_of_two) {
        return std::unexpected("GraphJit event capacity cannot be rounded to a power of two");
    }
    return next_power_of_2(required);
}

std::expected<std::size_t, std::string> append_event_representation(
    EventPortBindingPlan& plan,
    std::size_t group_index,
    EventTypeId type,
    std::size_t capacity,
    bool producer_telemetry = false,
    bool persistent = false,
    std::string migration_identity = {},
    bool persistent_ring = false,
    bool source_indices = false)
{
    if (capacity > std::numeric_limits<std::size_t>::max() / sizeof(TimedEvent)) {
        return std::unexpected("GraphJit event storage size overflows size_t");
    }
    auto const representation_index = plan.ports.size();
    plan.ports.push_back(EventRepresentationPlan{
        .producer_group_index = group_index,
        .type = type,
        .event_capacity = capacity,
        .persistent = persistent,
        .persistent_ring = persistent_ring,
        .migration_identity = owned_raw_region_identity(std::move(migration_identity)),
        .has_producer_overflow_counter = producer_telemetry,
        .has_source_indices = source_indices,
    });
    return representation_index;
}

std::expected<std::size_t, std::string> append_event_working_sequence(
    EventPortBindingPlan& plan,
    std::size_t group,
    EventTypeId type,
    std::size_t current,
    std::size_t retained,
    bool source_indices = false)
{
    if (current > std::numeric_limits<std::size_t>::max() - retained) {
        return std::unexpected("GraphJit Event working sequence capacity overflows size_t");
    }
    auto capacity = rounded_event_capacity(current + retained);
    if (!capacity) return std::unexpected(std::move(capacity.error()));
    return append_event_representation(
        plan, group, type, *capacity, false, false, {}, false, source_indices);
}

std::expected<void, std::string> realize_event_sequence_storage(
    EventPortBindingPlan& plan,
    EventSequenceStoragePlan const& sequence,
    RealtimeBufferStorageKind kind)
{
    auto& port = plan.ports[sequence.representation];
    auto migration_identity = [&](std::string_view storage_kind, std::size_t capacity) {
        auto identity = sequence.identity_base + ":kind=";
        if (sequence.ordered_fan_in) identity += "ordered_";
        identity += storage_kind;
        if (!sequence.feedback) {
            identity += ":history=" + std::to_string(sequence.history)
                + ":latency=" + std::to_string(sequence.latency);
        }
        return identity + ":capacity=" + std::to_string(capacity);
    };
    auto const retained = kind != RealtimeBufferStorageKind::transient_stack;
    if (sequence.append_merge && retained) {
        plan.merges[*sequence.append_merge].preserve_existing_target = true;
    }
    switch (kind) {
    case RealtimeBufferStorageKind::transient_stack:
        if (sequence.reset_transient) {
            plan.sequence_resets.push_back({
                .storage = sequence.representation,
                .scope = sequence.begin,
            });
        }
        break;
    case RealtimeBufferStorageKind::stack_with_persistent_carry: {
        auto capacity = rounded_event_capacity(sequence.retained_event_count);
        if (!capacity) return std::unexpected(std::move(capacity.error()));
        auto persistent = append_event_representation(
            plan, port.producer_group_index, port.type, *capacity,
            false, true, migration_identity("compact_carry", *capacity),
            false, port.has_source_indices);
        if (!persistent) return std::unexpected(std::move(persistent.error()));
        plan.carry_operations.push_back({
            .working_representation = sequence.representation,
            .persistent_representation = *persistent,
            .restore_scope = sequence.begin,
            .commit_scope = sequence.end,
            .retained_history_samples = sequence.history,
            .retained_latency_samples = sequence.latency,
        });
        break;
    }
    case RealtimeBufferStorageKind::full_node_storage:
        port.persistent = true;
        port.persistent_ring = true;
        port.migration_identity = owned_raw_region_identity(migration_identity(
            sequence.feedback ? "full_node_storage" : "persistent_ring",
            port.event_capacity));
        if (!sequence.feedback) {
            plan.persistent_rings.push_back({
                .storage = sequence.representation,
                .prune_scope = sequence.begin,
                .retained_history_samples = sequence.history,
            });
        }
        break;
    }
    return {};
}

// Callers match the source representation first, resolving any aliases.
// A pre-merge sequence must fit that merge's window. A callback-facing
// sequence can cover the largest history because each input reads its own
// window.
bool can_share_event_materialization(
    EventMaterializationPlan const& existing,
    EventMaterializationPlan const& requested)
{
    return existing.conversion == requested.conversion
        && existing.scope == requested.scope
        && existing.source_index_filter == requested.source_index_filter
        && existing.preserve_source_indices == requested.preserve_source_indices
        && existing.select_invocation_window == requested.select_invocation_window
        && existing.before_merge == requested.before_merge
        && (!existing.before_merge
            || existing.history_samples == requested.history_samples);
}

void extend_event_materialization(
    EventMaterializationPlan& existing,
    EventMaterializationPlan const& requested)
{
    existing.history_samples = std::max(existing.history_samples, requested.history_samples);
    existing.maximum_output_event_count = std::max(
        existing.maximum_output_event_count, requested.maximum_output_event_count);
    existing.maximum_source_event_reads = std::max(
        existing.maximum_source_event_reads, requested.maximum_source_event_reads);
}

std::expected<std::size_t, std::string> require_event_materialization(
    EventPortBindingPlan& plan,
    std::size_t group_index,
    EventMaterializationPlan requested)
{
    auto const existing = std::ranges::find_if(
        plan.materializations, [&](EventMaterializationPlan const& candidate) {
            return candidate.source_representation == requested.source_representation
                && can_share_event_materialization(candidate, requested);
        });
    if (existing != plan.materializations.end()) {
        extend_event_materialization(*existing, requested);
        return existing->target_representation;
    }

    auto target = append_event_representation(
        plan,
        group_index,
        requested.conversion.target_type,
        plan.ports[requested.source_representation].event_capacity,
        false,
        false,
        {},
        false,
        requested.preserve_source_indices);
    if (!target) return std::unexpected(std::move(target.error()));

    requested.target_representation = *target;
    plan.materializations.push_back(std::move(requested));
    return *target;
}

void apply_event_sequence_aliases(
    EventPortBindingPlan& plan, std::vector<std::optional<std::size_t>>& aliases)
{
    // A deferred raw-output alias needs neither a merge nor its own buffer.
    std::erase_if(plan.merges, [&](EventMergePlan const& merge) {
        return aliases[merge.target_representation].has_value();
    });
    // Several logical groups can use the same raw output. Share any
    // equivalent conversions of that output once the aliases are known.
    std::vector<std::uint8_t> aliased_sources(plan.ports.size());
    for (auto const alias : aliases) {
        if (alias) aliased_sources[*alias] = true;
    }
    std::vector<std::vector<std::size_t>> conversions_by_source(plan.ports.size());
    for (std::size_t index = 0; index < plan.materializations.size(); ++index) {
        auto& operation = plan.materializations[index];
        auto const source = aliases[operation.source_representation]
            .value_or(operation.source_representation);
        if (!aliased_sources[source]) continue;

        auto& conversions = conversions_by_source[source];
        auto const found = std::ranges::find_if(conversions, [&](std::size_t candidate_index) {
            auto const& candidate = plan.materializations[candidate_index];
            auto const& candidate_target = plan.ports[candidate.target_representation];
            auto const& target = plan.ports[operation.target_representation];
            return can_share_event_materialization(candidate, operation)
                && candidate_target.event_capacity == target.event_capacity;
        });
        if (found == conversions.end()) {
            conversions.push_back(index);
            continue;
        }
        auto& shared = plan.materializations[*found];
        aliases[operation.target_representation] = shared.target_representation;
        extend_event_materialization(shared, operation);
    }
    std::erase_if(plan.materializations, [&](EventMaterializationPlan const& operation) {
        return aliases[operation.target_representation].has_value();
    });

    std::vector<std::size_t> storage_indices(plan.ports.size());
    std::size_t storage_count = 0;
    for (std::size_t index = 0; index < plan.ports.size(); ++index) {
        if (aliases[index]) continue;
        storage_indices[index] = storage_count;
        if (storage_count != index) {
            plan.ports[storage_count] = std::move(plan.ports[index]);
        }
        ++storage_count;
    }
    plan.ports.resize(storage_count);
    auto resolve = [&](std::size_t& representation) {
        auto const resolved = aliases[representation].value_or(representation);
        representation = storage_indices[resolved];
    };
    auto resolve_optional = [&](std::optional<std::size_t>& representation) {
        if (representation) resolve(*representation);
    };
    for (auto& representation : plan.producer_group_representations) {
        resolve_optional(representation);
    }
    for (auto& sources : plan.producer_group_source_representations) {
        for (auto& representation : sources) resolve_optional(representation);
    }
    for (auto& primitive : plan.primitives) {
        for (auto& input : primitive.inputs) {
            resolve_optional(input.storage);
            resolve_optional(input.realtime_operand_storage);
            resolve_optional(input.combined_storage);
        }
        for (auto& output : primitive.outputs) {
            resolve_optional(output.storage);
            resolve_optional(output.canonical_storage);
        }
    }
    for (auto& operation : plan.materializations) {
        resolve(operation.source_representation);
        resolve(operation.target_representation);
    }
    for (auto& operation : plan.feedback_operations) {
        resolve(operation.source_representation);
        resolve(operation.target_representation);
    }
    for (auto& merge : plan.merges) {
        for (auto& source : merge.source_representations) resolve(source);
        resolve(merge.target_representation);
    }
    for (auto& carry : plan.carry_operations) {
        resolve(carry.working_representation);
        resolve(carry.persistent_representation);
    }
    for (auto& ring : plan.persistent_rings) resolve(ring.storage);
    for (auto& reset : plan.sequence_resets) resolve(reset.storage);
}

std::expected<void, std::string> realize_event_storage(
    EventPortBindingPlan& plan,
    std::vector<EventConnectionStorageRequirements> group_cost_requirements,
    std::vector<bool> const& group_requires_retention,
    std::span<EventSequenceStoragePlan const> group_sequences,
    std::span<EventSequenceStoragePlan const> feedback_sequences,
    RealtimeStorageCostModel const& cost_model)
{
    auto selected = select_event_storage(
        plan, std::move(group_cost_requirements), cost_model);
    if (!selected) return std::unexpected(std::move(selected.error()));
    plan.producer_group_storage_plans = std::move(selected->producer_groups);
    for (std::size_t group = 0; group < group_requires_retention.size(); ++group) {
        if (group_requires_retention[group]
            && plan.producer_group_storage_plans[group]->kind
                == RealtimeBufferStorageKind::transient_stack) {
            return std::unexpected("GraphJit event connection requires retained storage");
        }
    }

    std::vector<std::pair<std::size_t, std::size_t>> selected_aliases;
    for (auto const& sequence : group_sequences) {
        auto const group = plan.ports[sequence.representation].producer_group_index;
        auto const kind = plan.producer_group_storage_plans[group]->kind;
        if (sequence.transient_alias && kind == RealtimeBufferStorageKind::transient_stack) {
            selected_aliases.emplace_back(
                sequence.representation, *sequence.transient_alias);
            continue;
        }
        auto storage = realize_event_sequence_storage(plan, sequence, kind);
        if (!storage) return std::unexpected(std::move(storage.error()));
    }
    for (std::size_t index = 0; index < plan.feedback_operations.size(); ++index) {
        auto& operation = plan.feedback_operations[index];
        operation.storage_requirements = selected->feedback[index].requirements;
        operation.storage_plan = selected->feedback[index].storage;
        auto storage = realize_event_sequence_storage(
            plan, feedback_sequences[index], operation.storage_plan.kind);
        if (!storage) return std::unexpected(std::move(storage.error()));
    }
    if (!selected_aliases.empty()) {
        std::vector<std::optional<std::size_t>> aliases(plan.ports.size());
        for (auto const& [representation, raw] : selected_aliases) {
            aliases[representation] = raw;
        }
        apply_event_sequence_aliases(plan, aliases);
    }

    return {};
}

std::expected<EventPortBindingPlan, std::string> bind_event_ports(
    ConfiguredGraph const& graph,
    std::size_t block_size,
    GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections,
    ResolvedPortRealization const& realization,
    RealtimeStorageCostModel const& cost_model,
    EventPortInventory const& inventory)
{
    EventPortBindingPlan plan;

    auto primitive_index_for_bundle = [&](NodeBundleHandle bundle) -> std::optional<std::size_t> {
        return bundle < inventory.primitives.size() ? inventory.primitives[bundle] : std::nullopt;
    };

    plan.primitives.resize(analysis.primitives.size());
    for (std::size_t i = 0; i < analysis.primitives.size(); ++i) {
        auto const* node = inventory.nodes[analysis.primitives[i].bundle.node_bundle];
        if (!node) {
            return std::unexpected(
                "GraphJit event planning lost concrete-node port metadata");
        }
        if (analysis.primitives[i].implementation->tick_block.empty()) {
            // Tock ports use the caller-supplied background batch bindings.
            continue;
        }
        plan.primitives[i].inputs.resize(node->event_input_count);
        plan.primitives[i].outputs.resize(node->event_output_count);
        auto const bundle = analysis.primitives[i].bundle.node_bundle;
        for (std::size_t port = 0; port < node->event_output_count; ++port) {
            auto const config = graph.node_bundles.resolve_event_output(
                {bundle, PortKind::event, port}).config;
            plan.primitives[i].outputs[port].realtime =
                is_sequential_output(config);
        }
    }

    auto const group_count = connections.event_producer_groups.size();
    plan.producer_group_representations.resize(group_count);
    plan.producer_group_source_representations.resize(group_count);
    std::vector<EventConnectionStorageRequirements> group_cost_requirements(group_count);
    std::vector<EventSequenceStoragePlan> group_sequences;
    std::vector<EventSequenceStoragePlan> feedback_sequences;
    std::vector<bool> group_requires_retention(group_count);

    auto append_representation = [&] (
        std::size_t group_index,
        EventTypeId type,
        std::size_t capacity,
        bool producer_telemetry = false,
        bool persistent = false,
        std::string migration_identity = {},
        bool persistent_ring = false,
        bool source_indices = false) {
        return append_event_representation(
            plan, group_index, type, capacity, producer_telemetry, persistent,
            std::move(migration_identity), persistent_ring, source_indices);
    };

    // One read-only zero-count sequence per event type serves every
    // disconnected Sequential input of that type.
    std::vector<std::pair<EventTypeId, std::size_t>>
        immutable_empty_representations;
    auto immutable_empty_representation = [&](EventTypeId type)
        -> std::expected<std::size_t, std::string> {
        auto const found = std::ranges::find(
            immutable_empty_representations,
            type,
            &std::pair<EventTypeId, std::size_t>::first);
        if (found != immutable_empty_representations.end()) {
            return found->second;
        }
        auto representation = append_representation(
            connections.event_producer_groups.size(), type, 0);
        if (!representation) return representation;
        plan.ports[*representation].immutable_empty = true;
        immutable_empty_representations.emplace_back(type, *representation);
        return representation;
    };

    auto event_group_identity = [&](EventProducerGroupPlan const& group) {
        std::string identity =
            "graphjit.event:"
            + std::to_string(static_cast<unsigned>(group.source_type));
        for (std::size_t source_index = 0;
             source_index < group.sources.size(); ++source_index) {
            auto const source = group.sources[source_index];
            identity += ':' + raw_region_node_identity_text(connections, source.bundle)
                + '.' + std::to_string(source.port);
            if (source_index < group.source_conversions.size()) {
                auto const& conversion = group.source_conversions[source_index];
                identity += "[" + std::to_string(
                    static_cast<unsigned>(conversion.source_type));
                for (std::size_t step = 0;
                     step < conversion.step_count; ++step) {
                    identity += "," + std::to_string(
                        static_cast<unsigned>(conversion.steps[step]));
                }
                identity += ">" + std::to_string(
                    static_cast<unsigned>(conversion.target_type)) + "]";
            }
            // Groups of the same sources differ in their readers'
            // compensation, which places their events differently.
            if (source_index < group.source_compensations.size()
                && group.source_compensations[source_index] != 0) {
                identity += "+" + std::to_string(
                    group.source_compensations[source_index]);
            }
        }
        return identity;
    };

    auto region_for_bundle = [&](NodeBundleHandle bundle)
        -> SccRegionPlan const* {
        if (bundle >= connections.schedule.bundle_to_region.size()
            || !connections.schedule.bundle_to_region[bundle]) {
            return nullptr;
        }
        auto const region = *connections.schedule.bundle_to_region[bundle];
        return region < connections.schedule.regions.size()
            ? &connections.schedule.regions[region]
            : nullptr;
    };
    auto region_index_for_bundle = [&](NodeBundleHandle bundle)
        -> std::optional<std::size_t> {
        if (bundle >= connections.schedule.bundle_to_region.size()) {
            return std::nullopt;
        }
        return connections.schedule.bundle_to_region[bundle];
    };
    auto primitive_scope = [](std::size_t position, EventOperationPhase phase) {
        return EventOperationScope{
            .kind = EventOperationScopeKind::primitive,
            .phase = phase,
            .index = position,
        };
    };
    auto region_scope = [](std::size_t region, EventOperationPhase phase) {
        return EventOperationScope{
            .kind = EventOperationScopeKind::region,
            .phase = phase,
            .index = region,
        };
    };
    auto materialization_scope_for_target = [&] (
        std::span<EventOutputPortId const> sources,
        std::size_t producer_position,
        EventInputPortId target) -> std::expected<EventOperationScope, std::string> {
        std::optional<std::size_t> source_cyclic_region;
        for (auto const source : sources) {
            auto const region_index = region_index_for_bundle(source.bundle);
            if (!region_index
                || *region_index >= connections.schedule.regions.size()
                || !connections.schedule.regions[*region_index].cyclic) {
                source_cyclic_region.reset();
                break;
            }
            if (source_cyclic_region
                && *source_cyclic_region != *region_index) {
                source_cyclic_region.reset();
                break;
            }
            source_cyclic_region = *region_index;
        }
        auto const target_region = region_index_for_bundle(target.bundle);
        if (source_cyclic_region) {
            if (target_region && *target_region == *source_cyclic_region) {
                return primitive_scope(
                    producer_position, EventOperationPhase::after);
            }
            return region_scope(
                *source_cyclic_region, EventOperationPhase::after);
        }
        if (target_region
            && *target_region < connections.schedule.regions.size()
            && connections.schedule.regions[*target_region].cyclic) {
            return region_scope(*target_region, EventOperationPhase::before);
        }
        return primitive_scope(
            producer_position, EventOperationPhase::after);
    };
    struct EventMaterializationCostBound {
        std::size_t output_writes = 0;
        std::size_t source_reads = 0;
    };
    auto materialization_event_bound = [&] (
        double max_events_per_index,
        std::size_t history_samples,
        EventOperationScope scope,
        bool select_invocation_window,
        std::size_t complete_source_bound)
        -> std::expected<EventMaterializationCostBound, std::string> {
        if (!select_invocation_window) {
            return EventMaterializationCostBound{
                .output_writes = complete_source_bound,
                .source_reads = complete_source_bound,
            };
        }

        auto block_samples = block_size;
        std::size_t execution_count = 1;
        if (scope.kind == EventOperationScopeKind::primitive) {
            for (auto const& node : connections.nodes) {
                if (node.bundle
                        >= connections.schedule.bundle_execution_position.size()
                    || !connections.schedule.bundle_execution_position[
                        node.bundle]
                    || *connections.schedule.bundle_execution_position[
                        node.bundle] != scope.index) {
                    continue;
                }
                auto const region = region_index_for_bundle(node.bundle);
                if (region && *region < connections.schedule.regions.size()
                    && connections.schedule.regions[*region].cyclic) {
                    block_samples = connections.schedule.regions[*region]
                        .maximum_block_size;
                    if (block_samples == 0) {
                        return std::unexpected(
                            "GraphJit event materialization has no SCC quantum");
                    }
                    execution_count = block_size
                        / block_samples
                        + (block_size % block_samples != 0);
                }
                break;
            }
        }
        if (history_samples
            > std::numeric_limits<std::size_t>::max() - block_samples) {
            return std::unexpected(
                "GraphJit event materialization window overflows size_t");
        }
        std::size_t output_writes = 0;
        auto remaining = block_size;
        for (std::size_t execution = 0;
             execution < execution_count; ++execution) {
            auto const current_block = execution_count == 1
                ? block_samples
                : std::min(block_samples, remaining);
            auto const count = event_count_for_sample_span(
                max_events_per_index, current_block + history_samples);
            if (!count) {
                return std::unexpected(
                    "GraphJit event materialization bound is not representable");
            }
            output_writes = iv::detail::saturating_add(
                output_writes, std::min(*count, complete_source_bound));
            remaining -= current_block;
        }
        return EventMaterializationCostBound{
            .output_writes = output_writes,
            .source_reads = iv::detail::saturating_multiply(
                complete_source_bound, execution_count),
        };
    };

    // A concrete Tick event output has one callback-facing raw sequence even
    // when several connection fan-ins consume it. Fan-in groups own only
    // derived converted/merged sequences.
    auto ensure_raw_event_output = [&] (EventOutputPortId output)
        -> std::expected<std::size_t, std::string> {
        auto const primitive = primitive_index_for_bundle(output.bundle);
        if (!primitive
            || output.port >= plan.primitives[*primitive].outputs.size()) {
            return std::unexpected(
                "GraphJit event source requires an internal concrete output");
        }
        auto& binding = plan.primitives[*primitive].outputs[output.port];
        if (!binding.realtime) {
            return std::unexpected(
                "GraphJit realtime event source is not a Tick output");
        }
        if (binding.storage) {
            return *binding.storage;
        }
        if (output.bundle
                >= connections.schedule.bundle_execution_position.size()
            || !connections.schedule.bundle_execution_position[output.bundle]) {
            return std::unexpected(
                "GraphJit event source has no schedule position");
        }

        NodeBundlePortId const port_id{
            output.bundle, PortKind::event, output.port};
        auto const config = graph.node_bundles
            .resolve_event_output(port_id).config;
        if (!is_sequential_output(config)) {
            return std::unexpected(
                "GraphJit realtime event source disagrees with its declaration");
        }
        auto const& resolved_output = realization.event_output(
            output.bundle, output.port);
        auto const history = resolved_output.history;
        auto const latency = resolved_output.latency;
        if (history > std::numeric_limits<std::size_t>::max() - latency) {
            return std::unexpected(
                "GraphJit raw event output window overflows size_t");
        }
        auto const retained_window = history + latency;
        auto const region = region_index_for_bundle(output.bundle);
        auto const cyclic = region
            && *region < connections.schedule.regions.size()
            && connections.schedule.regions[*region].cyclic;
        auto callback_window = block_size;
        std::size_t aggregate_window = block_size;
        if (cyclic) {
            auto const quantum = connections.schedule.regions[*region]
                .maximum_block_size;
            if (quantum == 0) {
                return std::unexpected(
                    "GraphJit raw event output SCC has no execution quantum");
            }
            callback_window = std::min(
                block_size, quantum);
            auto const slice_count = block_size / quantum
                + (block_size % quantum != 0);
            if (retained_window != 0
                && slice_count
                    > (std::numeric_limits<std::size_t>::max()
                        - aggregate_window) / retained_window) {
                return std::unexpected(
                    "GraphJit raw event output aggregate window overflows size_t");
            }
            aggregate_window += slice_count * retained_window;
        } else {
            if (retained_window
                > std::numeric_limits<std::size_t>::max()
                    - aggregate_window) {
                return std::unexpected(
                    "GraphJit raw event output aggregate window overflows size_t");
            }
            aggregate_window += retained_window;
        }
        if (retained_window
            > std::numeric_limits<std::size_t>::max() - callback_window) {
            return std::unexpected(
                "GraphJit raw event callback window overflows size_t");
        }
        callback_window += retained_window;
        auto const current_events = event_count_for_sample_span(
            config.max_events_per_index, aggregate_window);
        auto const retained_events = event_count_for_sample_span(
            config.max_events_per_index, retained_window);
        auto const callback_current_events = event_count_for_sample_span(
            config.max_events_per_index, callback_window);
        auto const callback_events = event_sequence_capacity_for_sample_span(
            config.max_events_per_index, callback_window);
        if (!current_events || !retained_events || !callback_current_events
            || !callback_events
            || *current_events > std::numeric_limits<std::size_t>::max()
                    - *retained_events) {
            return std::unexpected(
                "GraphJit raw event output capacity is not representable");
        }
        // Retained storage must not be exposed to EventOutputPort because the
        // callback is allowed to author timestamps that overlap an earlier
        // invocation. Without history or latency, the callback sequence itself
        // is the raw EventOutputPortId representation. Cyclic fan-in operations
        // consume it after each primitive invocation and before the next slice
        // clears it.
        auto const retained_output = retained_window != 0;
        auto const canonical_current_events = retained_output
            ? *current_events
            : *callback_current_events;
        auto const requirements = EventConnectionStorageRequirements{
            .current_window_samples = retained_output
                ? block_size
                : callback_window,
            .retained_window_samples = retained_window,
            .current_event_capacity = canonical_current_events,
            .retained_event_capacity = *retained_events,
            .value_size_bytes = sizeof(TimedEvent),
            .operations = RealtimeStorageOperationCounts{
                .invariant_copied_values = retained_output
                    ? *current_events
                    : 0,
                .full_extra_copied_values = retained_output
                    ? 0
                    : *current_events,
                .full_ring_addressed_values = *current_events,
                .transient_extra_stack_values = retained_output
                    ? *callback_events
                    : 0,
                .carry_extra_stack_values = *callback_events,
                .full_extra_stack_values = *callback_events,
            },
        };
        auto const storage_plan = choose_event_connection_storage_plan(
            requirements, cost_model);
        auto const needs_callback_local = retained_output
            || storage_plan.kind != RealtimeBufferStorageKind::transient_stack;
        if (canonical_current_events
            > std::numeric_limits<std::size_t>::max() - *retained_events) {
            return std::unexpected(
                "GraphJit raw event output working capacity overflows size_t");
        }
        auto working_capacity = rounded_event_capacity(
            canonical_current_events + *retained_events);
        if (!working_capacity) {
            return std::unexpected(std::move(working_capacity.error()));
        }

        auto const owner = connections.event_producer_groups.size();
        auto const position = *connections.schedule.bundle_execution_position[
            output.bundle];
        auto const before_scope = cyclic
            ? region_scope(*region, EventOperationPhase::before)
            : primitive_scope(position, EventOperationPhase::before);
        auto const after_scope = cyclic
            ? region_scope(*region, EventOperationPhase::after)
            : primitive_scope(position, EventOperationPhase::after);
        auto const merge_scope = primitive_scope(
            position, EventOperationPhase::after);
        auto const identity_base =
            "graphjit.event.raw_output:"
            + raw_region_node_identity_text(connections, output.bundle) + "."
            + std::to_string(output.port)
            + ":history=" + std::to_string(history)
            + ":latency=" + std::to_string(latency);
        std::expected<std::size_t, std::string> canonical =
            std::unexpected(std::string{"unselected canonical event storage"});
        switch (storage_plan.kind) {
        case RealtimeBufferStorageKind::transient_stack:
            canonical = append_representation(
                owner,
                config.type,
                *working_capacity,
                !needs_callback_local);
            if (canonical && needs_callback_local) {
                plan.sequence_resets.push_back(EventSequenceResetPlan{
                    .storage = *canonical,
                    .scope = before_scope,
                });
            }
            break;
        case RealtimeBufferStorageKind::stack_with_persistent_carry: {
            canonical = append_representation(
                owner, config.type, *working_capacity);
            if (!canonical) break;
            auto carry_capacity = event_sequence_capacity_for_sample_span(
                config.max_events_per_index, retained_window);
            if (!carry_capacity) {
                return std::unexpected(
                    "GraphJit raw event output carry capacity is not representable");
            }
            auto persistent = append_representation(
                owner,
                config.type,
                *carry_capacity,
                false,
                true,
                identity_base + ":kind=compact_carry:capacity="
                    + std::to_string(*carry_capacity));
            if (!persistent) {
                return std::unexpected(std::move(persistent.error()));
            }
            plan.carry_operations.push_back(EventCarryPlan{
                .working_representation = *canonical,
                .persistent_representation = *persistent,
                .restore_scope = before_scope,
                .commit_scope = after_scope,
                .retained_history_samples = history,
                .retained_latency_samples = latency,
            });
            break;
        }
        case RealtimeBufferStorageKind::full_node_storage:
            canonical = append_representation(
                owner,
                config.type,
                *working_capacity,
                false,
                true,
                identity_base + ":kind=persistent_ring:capacity="
                    + std::to_string(*working_capacity),
                true);
            if (canonical) {
                plan.persistent_rings.push_back(EventPersistentRingPlan{
                    .storage = *canonical,
                    .prune_scope = before_scope,
                    .retained_history_samples = history,
                });
            }
            break;
        }
        if (!canonical) {
            return std::unexpected(std::move(canonical.error()));
        }
        auto callback_storage = *canonical;
        if (needs_callback_local) {
            auto local = append_representation(
                owner, config.type, *callback_events, true);
            if (!local) {
                return std::unexpected(std::move(local.error()));
            }
            callback_storage = *local;
            // The callback authors each event at its own position; the
            // canonical stream holds it where it becomes final, `latency`
            // positions later, which is where every consumer whose read delay
            // is the output's latency reads it.
            plan.merges.push_back(EventMergePlan{
                .source_representations = {callback_storage},
                .source_delays = {latency},
                .target_representation = *canonical,
                .scope = merge_scope,
                .ordering_history_samples = history,
                .preserve_existing_target = true,
                .before_materialization = true,
            });
        }
        binding = PrimitiveEventOutputBindingPlan{
            .realtime = true,
            .storage = callback_storage,
            .canonical_storage = *canonical,
            .source_type = config.type,
            .history = history,
            .latency = latency,
            .append_existing = !needs_callback_local
                    && !cyclic
                    && analysis.primitives[*primitive]
                           .bundle.maximum_block_size
                        < block_size,
        };
        return callback_storage;
    };
    auto source_conversions_match = [] (
        EventProducerGroupPlan const& group,
        EventConnectionPlan const& connection) {
        if (group.source_conversions.size()
            != connection.source_plans.size()) return false;
        for (std::size_t i = 0; i < group.source_conversions.size(); ++i) {
            if (group.source_conversions[i]
                != connection.source_plans[i].conversion) return false;
        }
        return true;
    };
    auto realtime_sources_match = [] (
        EventProducerGroupPlan const& group,
        EventConnectionPlan const& connection) {
        std::vector<EventOutputPortId> sources;
        std::vector<EventConversionPlan> conversions;
        for (std::size_t source_index = 0;
             source_index < connection.source_plans.size();
             ++source_index) {
            auto const realtime = std::ranges::any_of(
                connection.deliveries,
                [&](EventDeliveryPlan const& delivery) {
                    return delivery.source_index == source_index
                        && uses_realtime_storage(delivery.mechanism);
                });
            if (!realtime) continue;
            sources.push_back(connection.source_plans[source_index].source);
            conversions.push_back(
                connection.source_plans[source_index].conversion);
        }
        if (sources != group.realtime_sources
            || sources.size() != conversions.size()) {
            return false;
        }
        for (std::size_t index = 0; index < sources.size(); ++index) {
            auto const group_source = std::ranges::find(
                group.sources, sources[index]);
            if (group_source == group.sources.end()) return false;
            auto const group_index = static_cast<std::size_t>(
                std::distance(group.sources.begin(), group_source));
            if (group_index >= group.source_conversions.size()
                || group.source_conversions[group_index]
                    != conversions[index]) {
                return false;
            }
        }
        return true;
    };
    // The realtime targets of one of a group's connections that read the
    // group (EventProducerGroupPlan::connection_targets).
    auto group_targets = [&] (
        EventProducerGroupPlan const& group, std::size_t connection_index)
        -> std::vector<EventInputPortId> const& {
        static std::vector<EventInputPortId> const none{};
        for (std::size_t position = 0;
             position < group.connection_indices.size()
                && position < group.connection_targets.size();
             ++position) {
            if (group.connection_indices[position] == connection_index) {
                return group.connection_targets[position];
            }
        }
        return none;
    };
    // The largest compensation of any target of a connection
    // (EventProducerGroupPlan::source_compensations): how far ahead of the
    // block the group stream holds events its readers have not read yet.
    auto connection_compensation = [](EventConnectionPlan const& connection) {
        std::size_t result = 0;
        for (auto const& delivery : connection.deliveries) {
            if (!uses_realtime_storage(delivery.mechanism)
                || delivery.source_index >= connection.source_plans.size()) {
                continue;
            }
            auto const latency =
                connection.source_plans[delivery.source_index].latency;
            if (delivery.read_latency > latency) {
                result = std::max(result, delivery.read_latency - latency);
            }
        }
        return result;
    };
    // Mixed delivery is a property of one target: the connection may also
    // deliver other targets, for example Random Access inputs, entirely in
    // the background.
    auto target_receives_background_delivery = [](
        EventConnectionPlan const& connection, EventInputPortId target) {
        return std::ranges::any_of(
            connection.deliveries,
            [&](EventDeliveryPlan const& delivery) {
                return delivery.target_index < connection.target_plans.size()
                    && connection.target_plans[delivery.target_index].target
                        == target
                    && !uses_realtime_storage(delivery.mechanism);
            });
    };
    // A slot-fed input with history, and every mixed input, owns a
    // persistent timeline ring. The root inserts each delivered window into
    // it before the callback, so its history is the input's own record of
    // earlier deliveries in Tick invocation order.
    auto append_input_timeline = [&] (
        PrimitiveEventInputBindingPlan& binding,
        EventInputPortId target,
        EventTypeId type,
        std::size_t history,
        std::size_t required_capacity)
        -> std::expected<void, std::string> {
        auto const position = target.bundle
                < connections.schedule.bundle_execution_position.size()
            ? connections.schedule.bundle_execution_position[target.bundle]
            : std::nullopt;
        if (!position) {
            return std::unexpected(
                "GraphJit event input timeline has no execution position");
        }
        auto const capacity = rounded_event_capacity(
            std::max<std::size_t>(required_capacity, 1));
        if (!capacity) return std::unexpected(std::move(capacity.error()));
        auto migration_identity = input_timeline_migration_identity(
            connections.realtime_port_states,
            NodeBundlePortId{target.bundle, PortKind::event, target.port},
            "graphjit.event.input_timeline");
        if (!migration_identity) {
            return std::unexpected(std::move(migration_identity.error()));
        }
        if (!migration_identity->empty()) {
            *migration_identity += ":type="
                + std::to_string(static_cast<unsigned>(type))
                + ":capacity=" + std::to_string(*capacity);
        }
        auto timeline = append_representation(
            connections.event_producer_groups.size(),
            type,
            *capacity,
            false,
            true,
            std::move(*migration_identity),
            true);
        if (!timeline) return std::unexpected(std::move(timeline.error()));
        // Like a raw Tick output, a primitive sliced inside a cyclic region
        // prunes once before the region rather than before every slice.
        auto const region = region_index_for_bundle(target.bundle);
        auto const cyclic = region
            && *region < connections.schedule.regions.size()
            && connections.schedule.regions[*region].cyclic;
        plan.persistent_rings.push_back(EventPersistentRingPlan{
            .storage = *timeline,
            .prune_scope = cyclic
                ? region_scope(*region, EventOperationPhase::before)
                : primitive_scope(*position, EventOperationPhase::before),
            .retained_history_samples = history,
        });
        binding.storage = *timeline;
        binding.timeline_history_samples = history;
        return {};
    };
    auto configure_mixed_input = [&] (
        PrimitiveEventInputBindingPlan& binding,
        EventConnectionPlan const& connection,
        EventInputPortId target,
        std::size_t group_index,
        std::optional<std::size_t> fixed_realtime_source_index = {})
        -> std::expected<void, std::string> {
        auto const target_plan = std::ranges::find(
            connection.target_plans, target, &EventTargetPlan::target);
        if (target_plan == connection.target_plans.end()) {
            return std::unexpected(
                "GraphJit mixed event input lost its connection target");
        }
        if (!target_receives_background_delivery(connection, target)) {
            return {};
        }
        if (!binding.storage || *binding.storage >= plan.ports.size()) {
            return std::unexpected(
                "GraphJit mixed event input has no realtime representation");
        }
        if (target_plan->history
            > std::numeric_limits<std::size_t>::max()
                - block_size) {
            return std::unexpected(
                "GraphJit mixed event input window is not representable");
        }
        // The slot carries only the current window; the timeline also
        // retains the background contribution's history.
        auto const window_capacity = event_sequence_capacity_for_sample_span(
            connection.max_events_per_index,
            block_size);
        auto const retained_capacity = event_sequence_capacity_for_sample_span(
            connection.max_events_per_index,
            block_size + target_plan->history);
        auto const operand_capacity =
            plan.ports[*binding.storage].event_capacity;
        if (!window_capacity || !retained_capacity
            || operand_capacity
                > std::numeric_limits<std::size_t>::max()
                    - *retained_capacity) {
            return std::unexpected(
                "GraphJit mixed event input capacity is not representable");
        }
        auto const capacity = rounded_event_capacity(
            operand_capacity + *window_capacity);
        if (!capacity) return std::unexpected(std::move(capacity.error()));
        auto combined = append_representation(
            group_index, connection.target_type, *capacity);
        if (!combined) {
            return std::unexpected(std::move(combined.error()));
        }
        binding.invocation_supplied = true;
        binding.combines_invocation_supplied = true;
        binding.realtime_operand_storage = binding.storage;
        binding.combined_storage = *combined;
        binding.realtime_semantic_source_index = fixed_realtime_source_index;
        return append_input_timeline(
            binding,
            target,
            connection.target_type,
            target_plan->history,
            operand_capacity + *retained_capacity);
    };

    for (std::size_t group_index = 0;
         group_index < connections.event_producer_groups.size();
         ++group_index) {
        auto const& group = connections.event_producer_groups[group_index];
        // A group without realtime connections is delivered entirely by
        // background evaluation. Its Tick sources, if any, receive raw output
        // sinks below, which persisted capture reads.
        if (!group.has_realtime_connections) continue;
        if (group.external_boundary) {
            return std::unexpected(
                "GraphJit event planning has no internal realtime storage plan");
        }
        group_cost_requirements[group_index] = group.storage_requirements;

        plan.producer_group_source_representations[group_index].resize(
            group.sources.size());

        if (group.source_conversions.size() != group.sources.size()) {
            return std::unexpected(
                "GraphJit event producer group lost its source conversions");
        }

        if (group.realtime_sources.size() > 1) {
            std::optional<std::size_t> cyclic_source_region;
            bool has_acyclic_source = false;
            bool incompatible_cyclic_sources = false;
            for (auto const source : group.realtime_sources) {
                auto const region_index = region_index_for_bundle(source.bundle);
                if (!region_index
                    || *region_index >= connections.schedule.regions.size()
                    || !connections.schedule.regions[*region_index].cyclic) {
                    has_acyclic_source = true;
                    continue;
                }
                if (cyclic_source_region
                    && *cyclic_source_region != *region_index) {
                    incompatible_cyclic_sources = true;
                }
                cyclic_source_region = *region_index;
            }
            auto const staged_fan_in = incompatible_cyclic_sources
                || (cyclic_source_region && has_acyclic_source);

            // Producer streams are contractually time-sorted, but independent
            // producers cannot append into one sequence without disturbing
            // global order. Each EventOutputPortId therefore keeps its raw
            // callback sequence. Lowering converts those sequences as needed
            // and merges them into a separate fan-in sequence.
            struct ValidatedSource {
                EventOutputPortId id{};
                std::size_t semantic_source_index = 0;
                std::size_t primitive = 0;
                std::size_t execution_position = 0;
                EventOutputConfig config{};
                EventConversionPlan conversion{};
                std::size_t raw_representation = 0;
                std::size_t capacity = 0;
                std::size_t aggregate_event_bound = 0;
                std::optional<std::size_t> cyclic_region{};
                // The callback sequence holds events at their own positions;
                // the fan-in stream holds them where they become final.
                std::size_t latency = 0;
            };
            std::vector<ValidatedSource> validated_sources;
            validated_sources.reserve(group.realtime_sources.size());
            std::size_t total_local_capacity = 0;
            std::size_t total_invocation_local_capacity = 0;
            std::size_t producer_execution_position = 0;
            double observed_rate = 0.0;

            for (std::size_t group_source_index = 0;
                 group_source_index < group.sources.size();
                 ++group_source_index) {
                auto const source_id = group.sources[group_source_index];
                if (!std::ranges::contains(
                        group.realtime_sources, source_id)) {
                    continue;
                }
                auto const& source_conversion =
                    group.source_conversions[group_source_index];
                auto const source_primitive = primitive_index_for_bundle(
                    source_id.bundle);
                if (!source_primitive
                    || source_id.port
                        >= plan.primitives[*source_primitive].outputs.size()) {
                    return std::unexpected(
                        "GraphJit event fan-in requires internal concrete producers");
                }
                if (source_id.bundle
                        >= connections.schedule.bundle_execution_position.size()
                    || !connections.schedule.bundle_execution_position[
                        source_id.bundle]) {
                    return std::unexpected(
                        "GraphJit event fan-in producer is absent from the execution schedule");
                }
                NodeBundlePortId const source_port{
                    source_id.bundle, PortKind::event, source_id.port};
                auto const source = graph.node_bundles
                    .resolve_event_output(source_port).config;
                if (!is_sequential_output(source.production)
                    || source.type != source_conversion.source_type
                    || source_conversion.target_type != group.source_type) {
                    return std::unexpected(
                        "GraphJit event fan-in producer disagrees with its declaration");
                }
                auto raw_representation = ensure_raw_event_output(source_id);
                if (!raw_representation) {
                    return std::unexpected(
                        std::move(raw_representation.error()));
                }
                auto const& resolved_source =
                    realization.event_output(source_id.bundle, source_id.port);
                auto const source_history = resolved_source.history;
                auto const source_latency = resolved_source.latency;
                auto const source_region = region_index_for_bundle(
                    source_id.bundle);
                auto const source_cyclic_region = source_region
                        && *source_region < connections.schedule.regions.size()
                        && connections.schedule.regions[*source_region].cyclic
                    ? source_region
                    : std::optional<std::size_t>{};
                auto producer_window_samples = block_size;
                auto aggregate_window_samples = block_size;
                if (source_cyclic_region) {
                    auto const quantum = connections.schedule.regions[
                        *source_cyclic_region].maximum_block_size;
                    if (quantum == 0) {
                        return std::unexpected(
                            "GraphJit event fan-in source region has no SCC quantum");
                    }
                    auto const slice_count = block_size
                        / quantum
                        + (block_size % quantum != 0);
                    producer_window_samples = std::min(
                        block_size, quantum);
                    if (source_history
                            > std::numeric_limits<std::size_t>::max()
                                - source_latency
                        || ((source_history + source_latency) != 0
                            && slice_count
                                > (std::numeric_limits<std::size_t>::max()
                                    - aggregate_window_samples)
                                    / (source_history + source_latency))) {
                        return std::unexpected(
                            "GraphJit cyclic event fan-in authored window overflows size_t");
                    }
                    aggregate_window_samples += slice_count
                        * (source_history + source_latency);
                }
                if (source_history > std::numeric_limits<std::size_t>::max()
                        - producer_window_samples
                    || source_latency > std::numeric_limits<std::size_t>::max()
                        - producer_window_samples - source_history) {
                    return std::unexpected(
                        "GraphJit event fan-in producer temporal window overflows size_t");
                }
                producer_window_samples += source_history + source_latency;
                auto const capacity = event_sequence_capacity_for_sample_span(
                    source.max_events_per_index, producer_window_samples);
                auto const aggregate_event_bound = source_cyclic_region
                    ? event_count_for_sample_span(
                        source.max_events_per_index,
                        aggregate_window_samples)
                    : capacity;
                if (!capacity || !aggregate_event_bound
                    || *aggregate_event_bound
                        > std::numeric_limits<std::size_t>::max()
                        - total_local_capacity) {
                    return std::unexpected(
                        "GraphJit event fan-in producer capacity is not representable");
                }
                total_local_capacity += *aggregate_event_bound;
                if (source_conversion.size() != 0) {
                    if (*capacity
                        > std::numeric_limits<std::size_t>::max()
                            - total_invocation_local_capacity) {
                        return std::unexpected(
                            "GraphJit event fan-in conversion capacity overflows size_t");
                    }
                    total_invocation_local_capacity += *capacity;
                }
                observed_rate += source.max_events_per_index;
                if (!std::isfinite(observed_rate)) {
                    return std::unexpected(
                        "GraphJit event fan-in aggregate rate is not representable");
                }
                auto const execution_position =
                    *connections.schedule.bundle_execution_position[source_id.bundle];
                producer_execution_position = std::max(
                    producer_execution_position, execution_position);
                validated_sources.push_back(ValidatedSource{
                    .id = source_id,
                    .semantic_source_index = group_source_index,
                    .primitive = *source_primitive,
                    .execution_position = execution_position,
                    .config = source,
                    .conversion = source_conversion,
                    .raw_representation = *raw_representation,
                    .capacity = *capacity,
                    .aggregate_event_bound = *aggregate_event_bound,
                    .cyclic_region = source_cyclic_region,
                    .latency = source_latency,
                });
            }
            if (observed_rate != group.max_events_per_index) {
                return std::unexpected(
                    "GraphJit event fan-in aggregate max_events_per_index disagrees with connection analysis");
            }

            auto source_representation_for_group = [&] (
                ValidatedSource const& source,
                EventOperationScope scope)
                -> std::expected<std::size_t, std::string> {
                if (source.conversion.size() == 0) {
                    if (plan.ports[source.raw_representation].type
                        != group.source_type) {
                        return std::unexpected(
                            "GraphJit identity event source conversion changed type");
                    }
                    return source.raw_representation;
                }
                return require_event_materialization(
                    plan, group_index, EventMaterializationPlan{
                        .source_representation = source.raw_representation,
                        .conversion = source.conversion,
                        .maximum_output_event_count = source.aggregate_event_bound,
                        .maximum_source_event_reads = source.aggregate_event_bound,
                        .before_merge = true,
                        .scope = scope,
                    });
            };

            // Source indices let a composed connection select its ordinary
            // branches from retained fan-in storage. Detached branches read
            // the per-source representations recorded below. Output history
            // and latency remain in the raw EventOutputPortId representations.
            auto const requires_source_identity =
                std::ranges::any_of(
                    group.connection_indices,
                    [&](std::size_t connection_index) {
                        return connection_index
                                < connections.event_connections.size()
                            && has_realtime_delivery(
                                connections.event_connections[
                                    connection_index])
                            && (connections.event_connections[connection_index]
                                    .requires_source_composition
                                || !is_entirely_realtime_delivery(
                                    connections.event_connections[
                                        connection_index]));
                    })
                // Sources read at different compensation leave their readers
                // different in-flight windows, which a graph edit separates
                // by source.
                || std::ranges::adjacent_find(
                       group.source_compensations, std::ranges::not_equal_to{})
                    != group.source_compensations.end();

            std::size_t retained_history = 0;
            std::size_t retained_latency = 0;
            for (auto const connection_index : group.connection_indices) {
                if (connection_index >= connections.event_connections.size()) {
                    return std::unexpected(
                        "GraphJit event producer group references an invalid connection");
                }
                auto const& connection =
                    connections.event_connections[connection_index];
                if (!has_realtime_delivery(connection)) continue;
                retained_history = std::max(
                    retained_history,
                    std::max(connection.source_history, connection.target_history));
                // The fan-in stream holds each source's events where its
                // readers read them, compensation included.
                retained_latency = std::max(
                    retained_latency,
                    iv::detail::saturating_add(
                        connection.source_latency,
                        connection_compensation(connection)));
            }

            if (staged_fan_in) {
                struct FanInStage {
                    std::optional<std::size_t> cyclic_region{};
                    std::size_t execution_position = 0;
                    std::vector<std::size_t> source_indices{};
                };
                std::vector<FanInStage> stages;
                for (std::size_t source_index = 0;
                     source_index < validated_sources.size(); ++source_index) {
                    auto const& source = validated_sources[source_index];
                    auto stage = source.cyclic_region
                        ? std::ranges::find_if(
                            stages,
                            [&](FanInStage const& candidate) {
                                return candidate.cyclic_region
                                    == source.cyclic_region;
                            })
                        : stages.end();
                    if (stage == stages.end()) {
                        stages.push_back(FanInStage{
                            .cyclic_region = source.cyclic_region,
                            .execution_position = source.execution_position,
                            .source_indices = {source_index},
                        });
                    } else {
                        stage->execution_position = std::max(
                            stage->execution_position,
                            source.execution_position);
                        stage->source_indices.push_back(source_index);
                    }
                }
                std::ranges::sort(
                    stages,
                    {},
                    &FanInStage::execution_position);
                if (stages.empty()) {
                    return std::unexpected(
                        "GraphJit staged event fan-in has no execution stages");
                }

                std::size_t staged_local_stack = 0;
                for (auto const& source : validated_sources) {
                    if (source.conversion.size() == 0) continue;
                    auto const capacity = source.cyclic_region
                        ? source.capacity
                        : source.aggregate_event_bound;
                    staged_local_stack = iv::detail::saturating_add(
                        staged_local_stack, capacity);
                }

                struct StagedMergeCost {
                    std::size_t writes = 0;
                    std::size_t ring_accesses = 0;
                };
                auto staged_merge_cost = [&](std::size_t initial_values) {
                    auto target_values = initial_values;
                    StagedMergeCost result;
                    for (auto const& stage : stages) {
                        std::size_t executions = 1;
                        if (stage.cyclic_region) {
                            auto const quantum = connections.schedule.regions[
                                *stage.cyclic_region].maximum_block_size;
                            executions = block_size / quantum
                                + (block_size % quantum != 0);
                        }
                        std::vector<std::size_t> remaining;
                        remaining.reserve(stage.source_indices.size());
                        for (auto const source_index : stage.source_indices) {
                            remaining.push_back(
                                validated_sources[source_index]
                                    .aggregate_event_bound);
                        }
                        for (std::size_t execution = 0;
                             execution < executions; ++execution) {
                            for (std::size_t stage_source = 0;
                                 stage_source < stage.source_indices.size();
                                 ++stage_source) {
                                auto const& source = validated_sources[
                                    stage.source_indices[stage_source]];
                                auto const produced = std::min(
                                    source.cyclic_region
                                        ? source.capacity
                                        : source.aggregate_event_bound,
                                    remaining[stage_source]);
                                remaining[stage_source] -= produced;
                                auto const previous_target = target_values;
                                target_values = iv::detail::saturating_add(
                                    target_values, produced);
                                result.writes = iv::detail::saturating_add(
                                    result.writes, target_values);
                                result.ring_accesses = iv::detail::saturating_add(
                                    result.ring_accesses,
                                    iv::detail::saturating_add(
                                        previous_target, target_values));
                            }
                        }
                    }
                    return result;
                };

                {
                    auto const retained_values =
                        group.storage_requirements.retained_event_capacity;
                    auto const transient_merge = staged_merge_cost(0);
                    auto const retained_merge = staged_merge_cost(retained_values);
                    auto staged_requirements = group.storage_requirements;
                    staged_requirements.operations = RealtimeStorageOperationCounts{
                        .transient_extra_copied_values = transient_merge.writes,
                        .carry_extra_copied_values = retained_merge.writes,
                        .full_extra_copied_values = retained_merge.writes,
                        .full_ring_addressed_values =
                            retained_merge.ring_accesses,
                        .transient_extra_stack_values = staged_local_stack,
                        .carry_extra_stack_values = staged_local_stack,
                        .full_extra_stack_values = staged_local_stack,
                    };
                    group_cost_requirements[group_index] = staged_requirements;
                }
                if (retained_history > std::numeric_limits<std::size_t>::max()
                        - retained_latency) {
                    return std::unexpected(
                        "GraphJit staged event fan-in retained window overflows size_t");
                }
                auto const retained = event_count_for_sample_span(
                    group.max_events_per_index, retained_history + retained_latency);
                if (!retained) {
                    return std::unexpected(
                        "GraphJit staged event fan-in retained capacity is not representable");
                }
                auto canonical = append_event_working_sequence(
                    plan, group_index, group.source_type,
                    total_local_capacity, *retained, true);
                if (!canonical) {
                    return std::unexpected(std::move(canonical.error()));
                }
                plan.producer_group_representations[group_index] = *canonical;

                auto const first_scope = stages.front().cyclic_region
                    ? region_scope(
                        *stages.front().cyclic_region,
                        EventOperationPhase::before)
                    : primitive_scope(
                        stages.front().execution_position,
                        EventOperationPhase::before);
                auto const last_scope = stages.back().cyclic_region
                    ? region_scope(
                        *stages.back().cyclic_region,
                        EventOperationPhase::after)
                    : primitive_scope(
                        stages.back().execution_position,
                        EventOperationPhase::after);

                group_sequences.push_back({
                    .representation = *canonical,
                    .retained_event_count = *retained,
                    .begin = first_scope,
                    .end = last_scope,
                    .history = retained_history,
                    .latency = retained_latency,
                    .identity_base = event_group_identity(group),
                    .ordered_fan_in = true,
                    .reset_transient = true,
                });

                for (auto const& stage : stages) {
                    EventMergePlan merge{
                        .target_representation = *canonical,
                        .scope = primitive_scope(
                            stage.execution_position,
                            EventOperationPhase::after),
                        .ordering_history_samples = retained_history,
                        .preserve_existing_target = true,
                    };
                    merge.source_representations.reserve(
                        stage.source_indices.size());
                    merge.source_indices.reserve(stage.source_indices.size());
                    for (auto const source_index : stage.source_indices) {
                        auto const& source = validated_sources[source_index];
                        auto local = source_representation_for_group(
                            source, merge.scope);
                        if (!local) {
                            return std::unexpected(
                                std::move(local.error()));
                        }
                        plan.producer_group_source_representations[
                            group_index][source.semantic_source_index] = *local;
                        merge.source_representations.push_back(*local);
                        merge.source_indices.push_back(
                            source.semantic_source_index);
                        merge.source_delays.push_back(
                            iv::detail::saturating_add(
                                source.latency,
                                group.source_compensations.at(
                                    source.semantic_source_index)));
                    }
                    plan.merges.push_back(std::move(merge));
                }

                for (auto const connection_index : group.connection_indices) {
                    auto const& connection =
                        connections.event_connections[connection_index];
                    if (!has_realtime_delivery(connection)
                        || has_detached_source(connection)) continue;
                    auto const retained_connection =
                        connection.source_history != 0
                        || connection.source_latency != 0
                        || connection.target_history != 0
                        || connection_compensation(connection) != 0;
                    if (connection.external_boundary
                        || !realtime_sources_match(group, connection)
                        || connection.source_type != group.source_type
                        || connection.conversion.source_type
                            != connection.source_type
                        || connection.conversion.target_type
                            != connection.target_type) {
                        return std::unexpected(
                            "GraphJit staged event fan-in connection requires unsupported retention, feedback, external, or source semantics");
                    }
                    group_requires_retention[group_index] =
                        group_requires_retention[group_index] || retained_connection;

                    for (auto const target_id : group_targets(group, connection_index)) {
                        auto const target_primitive = primitive_index_for_bundle(
                            target_id.bundle);
                        if (!target_primitive
                            || target_id.port
                                >= plan.primitives[*target_primitive]
                                       .inputs.size()) {
                            return std::unexpected(
                                "GraphJit staged event fan-in requires internal concrete consumers");
                        }
                        NodeBundlePortId const target_port{
                            target_id.bundle, PortKind::event, target_id.port};
                        auto const target = graph.node_bundles
                            .resolve_event_input(target_port).config;
                        if (!is_sequential(target.access)
                            || target.type != connection.target_type) {
                            return std::unexpected(
                                "GraphJit staged event fan-in consumer disagrees with its declaration");
                        }

                        auto target_representation = *canonical;
                        if (connection.conversion.size() != 0) {
                            auto const preserve_source_indices =
                                target_receives_background_delivery(
                                    connection, target_id);
                            auto const target_region = region_index_for_bundle(
                                target_id.bundle);
                            EventOperationScope scope = last_scope;
                            if (target_region
                                && *target_region
                                    < connections.schedule.regions.size()
                                && connections.schedule.regions[*target_region]
                                       .cyclic) {
                                auto const source_stage = std::ranges::find_if(
                                    stages,
                                    [&](FanInStage const& candidate) {
                                        return candidate.cyclic_region
                                            == target_region;
                                    });
                                scope = source_stage == stages.end()
                                    ? region_scope(
                                        *target_region,
                                        EventOperationPhase::before)
                                    : primitive_scope(
                                        source_stage->execution_position,
                                        EventOperationPhase::after);
                            }
                            auto const select_invocation_window =
                                retained_connection
                                || (target_region
                                    && *target_region
                                        < connections.schedule.regions.size()
                                    && connections.schedule.regions[
                                           *target_region].cyclic);
                            auto materialized_events =
                                materialization_event_bound(
                                    group.max_events_per_index,
                                    connection.target_history,
                                    scope,
                                    select_invocation_window,
                                    iv::detail::saturating_add(
                                        total_local_capacity,
                                        group.storage_requirements
                                            .retained_event_capacity));
                            if (!materialized_events) {
                                return std::unexpected(
                                    std::move(materialized_events.error()));
                            }
                            auto converted = require_event_materialization(
                                plan, group_index, EventMaterializationPlan{
                                    .source_representation = *canonical,
                                    .conversion = connection.conversion,
                                    .preserve_source_indices = preserve_source_indices,
                                    .history_samples = connection.target_history,
                                    .maximum_output_event_count =
                                        materialized_events->output_writes,
                                    .maximum_source_event_reads =
                                        materialized_events->source_reads,
                                    .select_invocation_window = select_invocation_window,
                                    .scope = scope,
                                });
                            if (!converted) {
                                return std::unexpected(std::move(converted.error()));
                            }
                            target_representation = *converted;
                        }
                        auto& target_binding = plan.primitives[
                            *target_primitive].inputs[target_id.port];
                        if (target_binding.storage) {
                            return std::unexpected(
                                "GraphJit event input has more than one realized connection");
                        }
                        target_binding.storage = target_representation;
                        auto mixed = configure_mixed_input(
                            target_binding, connection, target_id, group_index);
                        if (!mixed) {
                            return std::unexpected(std::move(mixed.error()));
                        }
                    }
                }
                continue;
            }

            // Build and cost fan-in storage here, next to the exact raw
            // callback buffers and merge operation that lowering emits.
            // Connection analysis intentionally does not reconstruct these
            // implementation details.
            auto const saturating_add_values = [](std::size_t lhs,
                                                  std::size_t rhs) {
                return iv::detail::saturating_add(lhs, rhs);
            };
            auto const retained_values =
                group.storage_requirements.retained_event_capacity;

            std::size_t slice_count = 1;
            if (cyclic_source_region) {
                auto const quantum = connections.schedule.regions[
                    *cyclic_source_region].maximum_block_size;
                slice_count = block_size / quantum
                    + (block_size % quantum != 0);
            }

            auto sequential_merge_writes = [&](std::size_t initial_values) {
                auto target_values = initial_values;
                std::size_t writes = 0;
                if (!cyclic_source_region) {
                    for (std::size_t source_index = 0;
                         source_index < validated_sources.size(); ++source_index) {
                        target_values = saturating_add_values(
                            target_values,
                            validated_sources[source_index].aggregate_event_bound);
                        writes = saturating_add_values(writes, target_values);
                    }
                    return writes;
                }

                std::vector<std::size_t> remaining;
                remaining.reserve(validated_sources.size());
                for (auto const& source : validated_sources) {
                    remaining.push_back(source.aggregate_event_bound);
                }
                for (std::size_t slice = 0; slice < slice_count; ++slice) {
                    for (std::size_t source_index = 0;
                         source_index < validated_sources.size(); ++source_index) {
                        auto const produced = std::min(
                            validated_sources[source_index].capacity,
                            remaining[source_index]);
                        remaining[source_index] -= produced;
                        target_values = saturating_add_values(
                            target_values, produced);
                        writes = saturating_add_values(writes, target_values);
                    }
                }
                return writes;
            };

            auto sequential_ring_accesses = [&](std::size_t initial_values) {
                auto target_values = initial_values;
                std::size_t accesses = 0;
                auto account_source = [&](std::size_t produced) {
                    auto const previous_target = target_values;
                    target_values = saturating_add_values(
                        target_values, produced);
                    // event_sequence_merge() reads the existing target sequence
                    // and writes the complete merged target sequence. Source
                    // reads remain in raw callback stack buffers.
                    accesses = saturating_add_values(
                        accesses,
                        saturating_add_values(previous_target, target_values));
                };

                if (!cyclic_source_region) {
                    for (std::size_t source_index = 0;
                         source_index < validated_sources.size(); ++source_index) {
                        account_source(
                            validated_sources[source_index].aggregate_event_bound);
                    }
                    return accesses;
                }

                std::vector<std::size_t> remaining;
                remaining.reserve(validated_sources.size());
                for (auto const& source : validated_sources) {
                    remaining.push_back(source.aggregate_event_bound);
                }
                for (std::size_t slice = 0; slice < slice_count; ++slice) {
                    for (std::size_t source_index = 0;
                         source_index < validated_sources.size(); ++source_index) {
                        auto const produced = std::min(
                            validated_sources[source_index].capacity,
                            remaining[source_index]);
                        remaining[source_index] -= produced;
                        account_source(produced);
                    }
                }
                return accesses;
            };

            {
                auto const transient_merge_writes =
                    sequential_merge_writes(0);
                auto const retained_merge_writes =
                    sequential_merge_writes(retained_values);
                auto const retained_ring_accesses =
                    sequential_ring_accesses(retained_values);
                auto group_requirements = group.storage_requirements;
                group_requirements.operations = RealtimeStorageOperationCounts{
                    .transient_extra_copied_values = transient_merge_writes,
                    .carry_extra_copied_values = retained_merge_writes,
                    .full_extra_copied_values = retained_merge_writes,
                    .full_ring_addressed_values = retained_ring_accesses,
                    .transient_extra_stack_values =
                        total_invocation_local_capacity,
                    .carry_extra_stack_values = total_invocation_local_capacity,
                    .full_extra_stack_values = total_invocation_local_capacity,
                };
                group_cost_requirements[group_index] = group_requirements;
            }
            if (retained_history > std::numeric_limits<std::size_t>::max()
                    - retained_latency) {
                return std::unexpected(
                    "GraphJit event fan-in retained window overflows size_t");
            }
            auto const retained = event_count_for_sample_span(
                group.max_events_per_index, retained_history + retained_latency);
            if (!retained) {
                return std::unexpected(
                    "GraphJit event fan-in retained capacity is not representable");
            }
            auto const preserve_source_identity = requires_source_identity;
            auto canonical = append_event_working_sequence(
                plan, group_index, group.source_type, total_local_capacity,
                *retained, preserve_source_identity);
            if (!canonical) {
                return std::unexpected(std::move(canonical.error()));
            }
            plan.producer_group_representations[group_index] = *canonical;
            auto const restore_scope = cyclic_source_region
                ? region_scope(*cyclic_source_region, EventOperationPhase::before)
                : primitive_scope(producer_execution_position, EventOperationPhase::before);
            auto const commit_scope = cyclic_source_region
                ? region_scope(*cyclic_source_region, EventOperationPhase::after)
                : primitive_scope(producer_execution_position, EventOperationPhase::after);
            group_sequences.push_back({
                .representation = *canonical,
                .retained_event_count = *retained,
                .begin = restore_scope,
                .end = commit_scope,
                .history = retained_history,
                .latency = retained_latency,
                .identity_base = event_group_identity(group),
                .reset_transient = cyclic_source_region.has_value(),
                .append_merge = plan.merges.size(),
            });

            EventMergePlan merge{
                .target_representation = *canonical,
                .scope = primitive_scope(
                    producer_execution_position, EventOperationPhase::after),
                .ordering_history_samples = retained_history,
                .preserve_existing_target = cyclic_source_region.has_value(),
            };
            merge.source_representations.reserve(validated_sources.size());
            if (preserve_source_identity) {
                merge.source_indices.reserve(validated_sources.size());
            }
            for (std::size_t source_index = 0;
                 source_index < validated_sources.size(); ++source_index) {
                auto const& source = validated_sources[source_index];
                auto storage = source_representation_for_group(
                    source, merge.scope);
                if (!storage) {
                    return std::unexpected(std::move(storage.error()));
                }
                plan.producer_group_source_representations[
                    group_index][source.semantic_source_index] = *storage;
                merge.source_representations.push_back(*storage);
                merge.source_delays.push_back(
                    iv::detail::saturating_add(
                        source.latency,
                        group.source_compensations.at(
                            source.semantic_source_index)));
                if (preserve_source_identity) {
                    merge.source_indices.push_back(
                        source.semantic_source_index);
                }
            }
            plan.merges.push_back(std::move(merge));

            for (auto const connection_index : group.connection_indices) {
                auto const& connection =
                    connections.event_connections[connection_index];
                if (!has_realtime_delivery(connection)
                    || has_detached_source(connection)) continue;
                auto const retained_connection =
                    connection.source_history != 0
                    || connection.source_latency != 0
                    || connection.target_history != 0
                    || connection_compensation(connection) != 0;
                if (connection.external_boundary
                    || !realtime_sources_match(group, connection)
                    || connection.source_type != group.source_type
                    || connection.conversion.source_type
                        != connection.source_type
                    || connection.conversion.target_type
                        != connection.target_type) {
                    return std::unexpected(
                        "GraphJit event fan-in connection requires unsupported retention, feedback, external, or source semantics");
                }
                group_requires_retention[group_index] =
                    group_requires_retention[group_index] || retained_connection;

                for (auto const target_id : group_targets(group, connection_index)) {
                    auto const target_primitive = primitive_index_for_bundle(
                        target_id.bundle);
                    if (!target_primitive
                        || target_id.port
                            >= plan.primitives[*target_primitive].inputs.size()) {
                        return std::unexpected(
                            "GraphJit event fan-in requires internal concrete consumers");
                    }
                    NodeBundlePortId const target_port{
                        target_id.bundle, PortKind::event, target_id.port};
                    auto const target = graph.node_bundles
                        .resolve_event_input(target_port).config;
                    if (!is_sequential(target.access)
                        || target.type != connection.target_type) {
                        return std::unexpected(
                            "GraphJit event fan-in consumer disagrees with its declaration");
                    }
                    auto target_representation = *canonical;
                    if (connection.conversion.size() != 0) {
                        auto const preserve_source_indices =
                            target_receives_background_delivery(
                                connection, target_id);
                        auto scope = materialization_scope_for_target(
                            connection.sources,
                            producer_execution_position,
                            target_id);
                        if (!scope) {
                            return std::unexpected(std::move(scope.error()));
                        }
                        auto const* target_region = region_for_bundle(
                            target_id.bundle);
                        auto const select_invocation_window = retained_connection
                            || (target_region && target_region->cyclic);
                        auto materialized_events = materialization_event_bound(
                            group.max_events_per_index,
                            connection.target_history,
                            *scope,
                            select_invocation_window,
                            iv::detail::saturating_add(
                                total_local_capacity,
                                group.storage_requirements
                                    .retained_event_capacity));
                        if (!materialized_events) {
                            return std::unexpected(
                                std::move(materialized_events.error()));
                        }
                        auto converted = require_event_materialization(
                            plan, group_index, EventMaterializationPlan{
                                .source_representation = *canonical,
                                .conversion = connection.conversion,
                                .preserve_source_indices = preserve_source_indices,
                                .history_samples = connection.target_history,
                                .maximum_output_event_count =
                                    materialized_events->output_writes,
                                .maximum_source_event_reads =
                                    materialized_events->source_reads,
                                .select_invocation_window = select_invocation_window,
                                .scope = *scope,
                            });
                        if (!converted) {
                            return std::unexpected(std::move(converted.error()));
                        }
                        target_representation = *converted;
                    }
                    auto& target_binding =
                        plan.primitives[*target_primitive].inputs[target_id.port];
                    if (target_binding.storage) {
                        return std::unexpected(
                            "GraphJit event input has more than one realized connection");
                    }
                    target_binding.storage = target_representation;
                    auto mixed = configure_mixed_input(
                        target_binding, connection, target_id, group_index);
                    if (!mixed) {
                        return std::unexpected(std::move(mixed.error()));
                    }
                }
            }
            continue;
        }
        if (group.realtime_sources.size() != 1) {
            return std::unexpected(
                "GraphJit event producer group has no realtime source");
        }

        auto const source_id = group.realtime_sources.front();
        auto const source_primitive = primitive_index_for_bundle(source_id.bundle);
        if (!source_primitive) {
            return std::unexpected(
                "GraphJit event flow requires an internal concrete producer");
        }
        auto const base_storage = choose_event_connection_storage_plan(
            group.storage_requirements, cost_model);
        if (!group.requires_invocation_aggregate
            && base_storage.kind == RealtimeBufferStorageKind::transient_stack
            && analysis.primitives[*source_primitive].bundle.maximum_block_size
                < block_size) {
            return std::unexpected(
                "GraphJit direct event flow requires an unsliced producer");
        }
        NodeBundlePortId const source_port{
            source_id.bundle, PortKind::event, source_id.port};
        auto const source = graph.node_bundles
            .resolve_event_output(source_port).config;
        auto const source_position = std::ranges::find(
            group.sources, source_id);
        if (source_position == group.sources.end()) {
            return std::unexpected(
                "GraphJit realtime event source has no semantic position");
        }
        auto const semantic_source_index = static_cast<std::size_t>(
            std::distance(group.sources.begin(), source_position));
        auto const& source_conversion =
            group.source_conversions[semantic_source_index];
        if (!is_sequential_output(source.production)
            || source.type != source_conversion.source_type
            || source_conversion.target_type != group.source_type) {
            return std::unexpected(
                "GraphJit event producer disagrees with its declaration");
        }
        if (source_id.port >= plan.primitives[*source_primitive].outputs.size()) {
            return std::unexpected(
                "GraphJit event producer index is outside primitive metadata");
        }
        auto raw_source_representation = ensure_raw_event_output(source_id);
        if (!raw_source_representation) {
            return std::unexpected(
                std::move(raw_source_representation.error()));
        }
        auto const raw_canonical_representation = plan.primitives[
            *source_primitive].outputs[source_id.port]
                .canonical_storage.value_or(*raw_source_representation);

        if (source.max_events_per_index != group.max_events_per_index) {
            return std::unexpected(
                "GraphJit event producer max_events_per_index disagrees with connection analysis");
        }
        auto const source_region_index = region_index_for_bundle(source_id.bundle);
        auto const source_region_is_cyclic = source_region_index
            && *source_region_index < connections.schedule.regions.size()
            && connections.schedule.regions[*source_region_index].cyclic;
        auto const source_before_scope = source_region_is_cyclic
            ? region_scope(*source_region_index, EventOperationPhase::before)
            : primitive_scope(
                group.live_interval.begin, EventOperationPhase::before);
        auto const source_after_scope = source_region_is_cyclic
            ? region_scope(*source_region_index, EventOperationPhase::after)
            : primitive_scope(
                group.live_interval.begin, EventOperationPhase::after);
        auto const& resolved_source =
            realization.event_output(source_id.bundle, source_id.port);
        auto const source_history = resolved_source.history;
        auto const source_latency = resolved_source.latency;
        auto producer_window_samples = block_size;
        auto aggregate_window_samples = block_size;
        if (source_region_is_cyclic) {
            auto const quantum = connections.schedule.regions[
                *source_region_index].maximum_block_size;
            if (quantum == 0) {
                return std::unexpected(
                    "GraphJit event producer region has no SCC quantum");
            }
            auto const slice_count = block_size / quantum
                + (block_size % quantum != 0);
            producer_window_samples = std::min(
                block_size, quantum);
            if (source_history
                    > std::numeric_limits<std::size_t>::max() - source_latency
                || ((source_history + source_latency) != 0
                    && slice_count
                        > (std::numeric_limits<std::size_t>::max()
                            - aggregate_window_samples)
                            / (source_history + source_latency))) {
                return std::unexpected(
                    "GraphJit cyclic event producer authored window overflows size_t");
            }
            aggregate_window_samples += slice_count
                * (source_history + source_latency);
        }
        if (source_history > std::numeric_limits<std::size_t>::max()
                - producer_window_samples
            || source_latency > std::numeric_limits<std::size_t>::max()
                - producer_window_samples - source_history) {
            return std::unexpected(
                "GraphJit event producer temporal window overflows size_t");
        }
        producer_window_samples += source_history + source_latency;
        if (!source_region_is_cyclic) {
            aggregate_window_samples = producer_window_samples;
        }
        auto const base_max_events = event_count_for_sample_span(
            source.max_events_per_index, aggregate_window_samples);
        auto const base_capacity = event_sequence_capacity_for_sample_span(
            source.max_events_per_index, producer_window_samples);
        if (!base_max_events || !base_capacity) {
            return std::unexpected(
                "GraphJit event producer sizing rate/sample span exceeds representable static capacity");
        }

        std::size_t retained_history = 0;
        std::size_t retained_latency = 0;
        for (auto const connection_index : group.connection_indices) {
            if (connection_index >= connections.event_connections.size()) {
                return std::unexpected(
                    "GraphJit event producer group references an invalid connection");
            }
            auto const& connection = connections.event_connections[connection_index];
            if (!has_realtime_delivery(connection)) continue;
            retained_history = std::max(
                retained_history,
                std::max(connection.source_history, connection.target_history));
            // The group stream holds the source's events where its readers
            // read them, compensation included.
            retained_latency = std::max(
                retained_latency,
                iv::detail::saturating_add(
                    connection.source_latency,
                    connection_compensation(connection)));
        }
        auto const raw_has_retained_storage =
            plan.ports[raw_canonical_representation].persistent
            || std::ranges::any_of(
                plan.carry_operations,
                [&](EventCarryPlan const& carry) {
                    return carry.working_representation
                        == raw_canonical_representation;
                });
        // The group stream holds the source's events where its readers read
        // them: the raw output's own retained stream serves only readers
        // without compensation.
        auto const source_compensation =
            group.source_compensations.at(semantic_source_index);
        auto const can_alias_raw_source =
            !source_region_is_cyclic
            && source_conversion.size() == 0
            && source_compensation == 0
            && source_history >= retained_history
            && source_latency >= retained_latency;
        // Operations for a single-source group add the same copy costs to all
        // candidates and can only raise the cost of ring addressing. A
        // transient choice based on the base requirements is already final
        // for purposes of aliasing.
        auto const reuse_raw_source = can_alias_raw_source
            && (raw_has_retained_storage
                || base_storage.kind == RealtimeBufferStorageKind::transient_stack);
        auto source_representation = raw_canonical_representation;
        if (!reuse_raw_source) {
            if (retained_history > std::numeric_limits<std::size_t>::max()
                    - retained_latency) {
                return std::unexpected("GraphJit event producer retained window overflows size_t");
            }
            auto const retained = event_count_for_sample_span(
                source.max_events_per_index, retained_history + retained_latency);
            if (!retained) {
                return std::unexpected(
                    "GraphJit event producer retained capacity is not representable");
            }
            auto working = append_event_working_sequence(
                plan, group_index, group.source_type, *base_max_events,
                group.storage_requirements.retained_event_capacity);
            if (!working) return std::unexpected(std::move(working.error()));
            source_representation = *working;

            auto producer_representation = *raw_source_representation;
            auto const merge_scope = primitive_scope(
                group.live_interval.begin, EventOperationPhase::after);
            if (source_conversion.size() != 0) {
                auto converted = require_event_materialization(
                    plan, group_index, EventMaterializationPlan{
                        .source_representation = *raw_source_representation,
                        .conversion = source_conversion,
                        .maximum_output_event_count = *base_max_events,
                        .maximum_source_event_reads = *base_max_events,
                        .before_merge = true,
                        .scope = merge_scope,
                    });
                if (!converted) {
                    return std::unexpected(std::move(converted.error()));
                }
                producer_representation = *converted;
            }
            group_sequences.push_back({
                .representation = source_representation,
                .retained_event_count = *retained,
                .begin = source_before_scope,
                .end = source_after_scope,
                .history = retained_history,
                .latency = retained_latency,
                .identity_base = "graphjit.event:"
                    + std::to_string(static_cast<unsigned>(group.source_type))
                    + ':' + raw_region_node_identity_text(connections, source_id.bundle)
                    + '.' + std::to_string(source_id.port),
                .reset_transient = true,
                .transient_alias = can_alias_raw_source
                        && base_storage.candidate_costs.transient_stack.legal
                    ? std::optional(raw_canonical_representation) : std::nullopt,
                .append_merge = plan.merges.size(),
            });
            plan.merges.push_back(EventMergePlan{
                .source_representations = {producer_representation},
                .source_delays = {iv::detail::saturating_add(
                    source_latency, source_compensation)},
                .target_representation = source_representation,
                .scope = merge_scope,
                .ordering_history_samples = retained_history,
                .preserve_existing_target = source_region_is_cyclic,
            });
        }
        plan.producer_group_representations[group_index] = source_representation;

        for (auto const connection_index : group.connection_indices) {
            auto const& connection = connections.event_connections[connection_index];
            if (!has_realtime_delivery(connection)
                || has_detached_source(connection)) continue;
            auto const retained_connection =
                connection.source_history != 0
                || connection.source_latency != 0
                || connection.target_history != 0
                || connection_compensation(connection) != 0;
            if (connection.external_boundary
                || !realtime_sources_match(group, connection)
                || connection.source_type != group.source_type
                || connection.conversion.source_type != connection.source_type
                || connection.conversion.target_type != connection.target_type) {
                return std::unexpected(
                    "GraphJit event connection requires unsupported retention, feedback, external, or source-composition semantics");
            }
            group_requires_retention[group_index] =
                group_requires_retention[group_index] || retained_connection;
            for (auto const target_id : group_targets(group, connection_index)) {
                auto const target_primitive = primitive_index_for_bundle(
                    target_id.bundle);
                if (!target_primitive) {
                    return std::unexpected(
                        "GraphJit event flow requires internal concrete consumers");
                }
                NodeBundlePortId const target_port{
                    target_id.bundle, PortKind::event, target_id.port};
                auto const target = graph.node_bundles
                    .resolve_event_input(target_port).config;
                if (!is_sequential(target.access)
                    || target.type != connection.target_type
                    || target_id.port
                        >= plan.primitives[*target_primitive].inputs.size()) {
                    return std::unexpected(
                        "GraphJit event consumer disagrees with its declaration");
                }
                auto target_representation = source_representation;
                if (connection.conversion.size() != 0) {
                    auto scope = materialization_scope_for_target(
                        connection.sources,
                        group.live_interval.begin,
                        target_id);
                    if (!scope) {
                        return std::unexpected(std::move(scope.error()));
                    }
                    auto const target_region = region_for_bundle(
                        target_id.bundle);
                    auto const select_invocation_window = retained_connection
                        || (target_region && target_region->cyclic);
                    auto materialized_events = materialization_event_bound(
                        group.max_events_per_index,
                        connection.target_history,
                        *scope,
                        select_invocation_window,
                        iv::detail::saturating_add(
                            *base_max_events,
                            group.storage_requirements
                                .retained_event_capacity));
                    if (!materialized_events) {
                        return std::unexpected(
                            std::move(materialized_events.error()));
                    }
                    auto converted = require_event_materialization(
                        plan, group_index, EventMaterializationPlan{
                            .source_representation = source_representation,
                            .conversion = connection.conversion,
                            .history_samples = connection.target_history,
                            .maximum_output_event_count =
                                materialized_events->output_writes,
                            .maximum_source_event_reads =
                                materialized_events->source_reads,
                            .select_invocation_window = select_invocation_window,
                            .scope = *scope,
                        });
                    if (!converted) {
                        return std::unexpected(std::move(converted.error()));
                    }
                    target_representation = *converted;
                }
                auto& target_binding =
                    plan.primitives[*target_primitive].inputs[target_id.port];
                if (target_binding.storage) {
                    return std::unexpected(
                        "GraphJit event input has more than one realized connection");
                }
                target_binding.storage = target_representation;
                auto mixed = configure_mixed_input(
                    target_binding,
                    connection,
                    target_id,
                    group_index,
                    semantic_source_index);
                if (!mixed) {
                    return std::unexpected(std::move(mixed.error()));
                }
            }
        }
    }

    auto plan_source_event_feedback = [&] (
        EventConnectionPlan const& connection,
        EventSourcePlan const& source,
        std::size_t source_index,
        std::size_t source_group_index,
        std::size_t source_representation,
        std::size_t producer_position)
        -> std::expected<std::size_t, std::string> {
        if (!source.detach || !source.detach_region
            || *source.detach_region >= connections.schedule.regions.size()
            || source.detach->loop_extra_latency == 0) {
            return std::unexpected(
                "GraphJit per-source event detach lost its validated SCC metadata");
        }
        auto const& region =
            connections.schedule.regions[*source.detach_region];
        if (!region.cyclic || region.maximum_block_size == 0
            || source_group_index
                >= connections.event_producer_groups.size()
            || source_representation >= plan.ports.size()) {
            return std::unexpected(
                "GraphJit per-source event detach has invalid producer storage");
        }
        auto const& group =
            connections.event_producer_groups[source_group_index];
        auto const& source_storage = plan.ports[source_representation];
        if (source_storage.type != connection.source_type) {
            return std::unexpected(
                "GraphJit per-source event detach source representation changed type");
        }

        auto const detach_latency = source.detach->loop_extra_latency;
        if (source.latency
            > std::numeric_limits<std::size_t>::max() - detach_latency) {
            return std::unexpected(
                "GraphJit per-source event feedback delay overflows size_t");
        }
        // The callback-local source sequence holds each event at its own
        // position. The input reads it at the source latency plus the detach
        // latency.
        auto const latency = source.latency + detach_latency;
        auto const retained_history = std::max(
            source.history, connection.target_history);
        if (retained_history
                > std::numeric_limits<std::size_t>::max() - latency) {
            return std::unexpected(
                "GraphJit per-source event feedback retained span overflows size_t");
        }
        auto const retained_window_samples = retained_history + latency;
        auto const slice_count = block_size
            / region.maximum_block_size
            + (block_size % region.maximum_block_size != 0);
        if (source.history
                > std::numeric_limits<std::size_t>::max() - source.latency
            || ((source.history + source.latency) != 0
                && slice_count
                    > (std::numeric_limits<std::size_t>::max()
                        - block_size)
                        / (source.history + source.latency))) {
            return std::unexpected(
                "GraphJit per-source event feedback authored span overflows size_t");
        }
        auto const authored_window_samples = block_size
            + slice_count * (source.history + source.latency);
        auto const current_event_count = event_count_for_sample_span(
            source.max_events_per_index, block_size);
        auto const retained_event_count = event_count_for_sample_span(
            source.max_events_per_index, retained_window_samples);
        auto const authored_event_count = event_count_for_sample_span(
            source.max_events_per_index, authored_window_samples);
        auto const source_event_reads = event_count_for_sample_span(
            source.max_events_per_index, authored_window_samples);
        if (!current_event_count || !retained_event_count
            || !authored_event_count || !source_event_reads) {
            return std::unexpected(
                "GraphJit per-source event feedback rate/span is not representable");
        }
        auto feedback_requirements = EventConnectionStorageRequirements{
            .current_window_samples = block_size,
            .retained_window_samples = retained_window_samples,
            .current_event_capacity = *current_event_count,
            .retained_event_capacity = *retained_event_count,
            .value_size_bytes = sizeof(TimedEvent),
            .operations = RealtimeStorageOperationCounts{
                .invariant_copied_values = *authored_event_count,
                .full_ring_addressed_values = *authored_event_count,
            },
        };
        auto const append_scope = primitive_scope(
            producer_position, EventOperationPhase::after);
        auto const reset_scope = region_scope(
            *source.detach_region, EventOperationPhase::before);
        auto existing = std::ranges::find_if(
            plan.feedback_operations,
            [&](EventFeedbackPlan const& feedback) {
                return feedback.source_representation == source_representation
                    && !feedback.source_index_filter
                    && feedback.source_sequence_restarts_each_append
                    && feedback.append_scope == append_scope
                    && feedback.reset_scope == reset_scope
                    && feedback.loop_extra_latency == latency
                    && feedback.retained_window_samples
                        == retained_window_samples;
            });
        if (existing != plan.feedback_operations.end())
            return existing->target_representation;

        auto target = append_event_working_sequence(
            plan, source_group_index, connection.source_type, *authored_event_count,
            *retained_event_count);
        if (!target) return std::unexpected(std::move(target.error()));
        auto const target_representation = *target;
        feedback_sequences.push_back({
            .representation = target_representation,
            .retained_event_count = *retained_event_count,
            .begin = reset_scope,
            .end = region_scope(*source.detach_region, EventOperationPhase::after),
            .history = retained_history,
            .latency = latency,
            .identity_base = "graphjit.event.feedback:group=" + event_group_identity(group)
                + ":source=" + std::to_string(source_index)
                + ":type=" + std::to_string(static_cast<unsigned>(connection.source_type))
                + ":latency=" + std::to_string(latency)
                + ":retained=" + std::to_string(retained_window_samples),
            .feedback = true,
        });
        plan.feedback_operations.push_back(EventFeedbackPlan{
            .source_representation = source_representation,
            .target_representation = target_representation,
            .source_sequence_restarts_each_append = true,
            .append_scope = append_scope,
            .reset_scope = reset_scope,
            .storage_requirements = feedback_requirements,
            .authored_event_count = *authored_event_count,
            .source_event_reads = *source_event_reads,
            .retained_event_count = *retained_event_count,
            .retained_window_samples = retained_window_samples,
            .ordering_history_samples = retained_history,
            .loop_extra_latency = latency,
        });
        return target_representation;
    };

    for (auto const& connection : connections.event_connections) {
        if (!connection.detach) continue;
        if (!connection.detach_region
            || *connection.detach_region >= connections.schedule.regions.size()
            || !connection.detach->loop_extra_latency) {
            return std::unexpected(
                "GraphJit event detach lost its validated SCC metadata");
        }
        auto const& region = connections.schedule.regions[*connection.detach_region];
        if (!region.cyclic || region.maximum_block_size == 0) {
            return std::unexpected(
                "GraphJit event detach has an invalid SCC execution region");
        }
        if (!is_entirely_realtime_delivery(connection)
            || connection.external_boundary) {
            return std::unexpected(
                "GraphJit event feedback requires internal realtime transport");
        }

        auto const source_group_it = std::ranges::find_if(
            connections.event_producer_groups,
            [&](EventProducerGroupPlan const& group) {
                // Groups also differ in their readers' compensation; the
                // connection's own group is the one that lists it.
                auto const connection_index = static_cast<std::size_t>(
                    &connection - connections.event_connections.data());
                return group.source_type == connection.source_type
                    && group.sources == connection.sources
                    && source_conversions_match(group, connection)
                    && std::ranges::contains(
                        group.connection_indices, connection_index);
            });
        if (source_group_it == connections.event_producer_groups.end()) {
            return std::unexpected(
                "GraphJit event detach lost its semantic producer group");
        }
        auto const source_group_index = static_cast<std::size_t>(
            std::distance(connections.event_producer_groups.begin(), source_group_it));
        std::size_t producer_position = 0;
        for (auto const source : connection.sources) {
            if (source.bundle
                    >= connections.schedule.bundle_execution_position.size()
                || !connections.schedule.bundle_execution_position[source.bundle]) {
                return std::unexpected(
                    "GraphJit event feedback source has no executable schedule position");
            }
            producer_position = std::max(
                producer_position,
                *connections.schedule.bundle_execution_position[source.bundle]);
        }
        if (source_group_index >= plan.producer_group_representations.size()
            || !plan.producer_group_representations[source_group_index]) {
            return std::unexpected(
                "GraphJit event detach source has no realized event storage");
        }
        auto const canonical_source_representation =
            *plan.producer_group_representations[source_group_index];
        if (canonical_source_representation >= plan.ports.size()) {
            return std::unexpected(
                "GraphJit event detach source storage is invalid");
        }
        // Detached feedback observes the producer group's canonical root-call
        // stream, which holds each event where it becomes final. Any local
        // source-history/latency buffer is merged into this stream immediately
        // before feedback_append runs, which then copies the events that became
        // final during the current slice. Each event is delivered once, at its
        // final position plus the detach latency.
        auto const source_representation = canonical_source_representation;
        auto const& source_storage = plan.ports[source_representation];
        if (source_storage.type != connection.source_type) {
            return std::unexpected(
                "GraphJit event detach source storage disagrees with detach type");
        }

        auto const latency = connection.detach->loop_extra_latency;
        bool has_consumer_position = false;
        for (auto const target_id : connection.targets) {
            if (target_id.bundle
                    >= connections.schedule.bundle_execution_position.size()
                || !connections.schedule.bundle_execution_position[target_id.bundle]) {
                return std::unexpected(
                    "GraphJit event feedback consumer has no executable schedule position");
            }
            has_consumer_position = true;
        }
        if (!has_consumer_position) {
            return std::unexpected(
                "GraphJit event feedback has no in-SCC consumer");
        }

        auto const retained_history = std::max(
            connection.source_history, connection.target_history);
        if (connection.source_latency
                > std::numeric_limits<std::size_t>::max() - latency
            || retained_history
                > std::numeric_limits<std::size_t>::max()
                    - latency - connection.source_latency) {
            return std::unexpected(
                "GraphJit event feedback retained span overflows size_t");
        }
        auto const retained_window_samples =
            retained_history + connection.source_latency + latency;
        auto const slice_count = block_size
            / region.maximum_block_size
            + (block_size % region.maximum_block_size != 0);
        if (connection.source_history
                > std::numeric_limits<std::size_t>::max()
                    - connection.source_latency
            || ((connection.source_history + connection.source_latency) != 0
                && slice_count
                    > (std::numeric_limits<std::size_t>::max()
                        - block_size)
                        / (connection.source_history
                            + connection.source_latency))) {
            return std::unexpected(
                "GraphJit event feedback authored span overflows size_t");
        }
        auto const authored_window_samples =
            block_size
            + slice_count
                * (connection.source_history + connection.source_latency);
        auto const current_event_count = event_count_for_sample_span(
            source_group_it->max_events_per_index,
            block_size);
        auto const retained_event_count = event_count_for_sample_span(
            source_group_it->max_events_per_index,
            retained_window_samples);
        auto const authored_event_count = event_count_for_sample_span(
            source_group_it->max_events_per_index,
            authored_window_samples);
        if (!current_event_count || !retained_event_count
            || !authored_event_count) {
            return std::unexpected(
                "GraphJit event feedback rate/span exceeds representable static capacity");
        }
        auto feedback_requirements = EventConnectionStorageRequirements{
                // Storage selection compares the root-call working window with
                // the state retained between calls. History/latency may make a
                // feedback producer author more than one root window while its
                // SCC is sliced; that additional work belongs in the operation
                // counts below, not in the current-footprint term. Storage
                // feedback buffers are still sized from authored_event_count.
                .current_window_samples = block_size,
                .retained_window_samples = retained_window_samples,
                .current_event_capacity = *current_event_count,
                .retained_event_capacity = *retained_event_count,
                .value_size_bytes = sizeof(TimedEvent),
                .operations = RealtimeStorageOperationCounts{
                    // feedback_append copies the newly-authored producer
                    // suffix exactly once. A full persistent target performs
                    // the same writes through ring addressing.
                    .invariant_copied_values = *authored_event_count,
                    .full_ring_addressed_values = *authored_event_count,
                },
            };

        // Multiple detached branches from one source with the same authored
        // latency are the same delayed event stream. Share one planned delayed
        // storage and one producer-side append operation across all such
        // consumers, regardless of whether that storage is transient,
        // compact carry, or a full persistent ring.
        auto existing_feedback = std::ranges::find_if(
            plan.feedback_operations,
            [&](EventFeedbackPlan const& feedback) {
                return feedback.source_representation == source_representation
                    && feedback.append_scope
                        == primitive_scope(
                            producer_position, EventOperationPhase::after)
                    && feedback.loop_extra_latency == latency
                    && feedback.retained_window_samples == retained_window_samples;
            });

        std::size_t target_representation = 0;
        if (existing_feedback != plan.feedback_operations.end()) {
            target_representation = existing_feedback->target_representation;
            if (existing_feedback->reset_scope
                != region_scope(
                    *connection.detach_region, EventOperationPhase::before)) {
                return std::unexpected(
                    "GraphJit shared event feedback has an invalid reset scope");
            }
        } else {
            auto const reset_scope = region_scope(
                *connection.detach_region, EventOperationPhase::before);
            auto target = append_event_working_sequence(
                plan, source_group_index, connection.source_type, *authored_event_count,
                *retained_event_count);
            if (!target) return std::unexpected(std::move(target.error()));
            target_representation = *target;
            feedback_sequences.push_back({
                .representation = target_representation,
                .retained_event_count = *retained_event_count,
                .begin = reset_scope,
                .end = region_scope(*connection.detach_region, EventOperationPhase::after),
                .history = retained_history,
                .latency = connection.source_latency + latency,
                .identity_base = "graphjit.event.feedback:group="
                    + event_group_identity(*source_group_it)
                    + ":type=" + std::to_string(static_cast<unsigned>(connection.source_type))
                    + ":latency=" + std::to_string(latency)
                    + ":retained=" + std::to_string(retained_window_samples),
                .feedback = true,
            });

            plan.feedback_operations.push_back(EventFeedbackPlan{
                .source_representation = source_representation,
                .target_representation = target_representation,
                .selects_final_window = true,
                .append_scope = primitive_scope(
                    producer_position, EventOperationPhase::after),
                .reset_scope = region_scope(
                    *connection.detach_region, EventOperationPhase::before),
                .storage_requirements = feedback_requirements,
                .authored_event_count = *authored_event_count,
                .source_event_reads = *authored_event_count,
                .retained_event_count = *retained_event_count,
                .retained_window_samples = retained_window_samples,
                .ordering_history_samples = retained_history,
                .loop_extra_latency = latency,
            });
        }

        for (auto const target_id : connection.targets) {
            auto const target_primitive = primitive_index_for_bundle(target_id.bundle);
            if (!target_primitive
                || target_id.port >= plan.primitives[*target_primitive].inputs.size()) {
                return std::unexpected(
                    "GraphJit event feedback requires concrete in-SCC consumers");
            }
            NodeBundlePortId const target_port{
                target_id.bundle, PortKind::event, target_id.port};
            auto const target = graph.node_bundles
                .resolve_event_input(target_port).config;
            if (!is_sequential(target.access)
                || target.type != connection.target_type) {
                return std::unexpected(
                    "GraphJit event feedback consumer disagrees with its declaration");
            }
            auto consumer_representation = target_representation;
            if (connection.conversion.size() != 0) {
                if (target_id.bundle
                        >= connections.schedule.bundle_execution_position.size()
                    || !connections.schedule.bundle_execution_position[
                        target_id.bundle]) {
                    return std::unexpected(
                        "GraphJit event feedback consumer has no schedule position");
                }
                auto const scope = primitive_scope(
                    *connections.schedule.bundle_execution_position[
                        target_id.bundle],
                    EventOperationPhase::before);
                auto materialized_events = materialization_event_bound(
                    source_group_it->max_events_per_index,
                    connection.target_history,
                    scope,
                    true,
                    iv::detail::saturating_add(
                        *authored_event_count, *retained_event_count));
                if (!materialized_events) {
                    return std::unexpected(
                        std::move(materialized_events.error()));
                }
                auto converted = require_event_materialization(
                    plan, source_group_index, EventMaterializationPlan{
                        .source_representation = target_representation,
                        .conversion = connection.conversion,
                        .history_samples = connection.target_history,
                        .maximum_output_event_count =
                            materialized_events->output_writes,
                        .maximum_source_event_reads =
                            materialized_events->source_reads,
                        .select_invocation_window = true,
                        .scope = scope,
                    });
                if (!converted) {
                    return std::unexpected(std::move(converted.error()));
                }
                consumer_representation = *converted;
            }
            auto& target_binding =
                plan.primitives[*target_primitive].inputs[target_id.port];
            if (target_binding.storage) {
                return std::unexpected(
                    "GraphJit event feedback consumer input is connected more than once");
            }
            target_binding.storage = consumer_representation;
        }
    }

    for (auto const& connection : connections.event_connections) {
        if (!connection.requires_source_composition) continue;
        if (!is_entirely_realtime_delivery(connection)
            || connection.external_boundary) {
            return std::unexpected(
                "GraphJit composed event detach requires internal realtime transport");
        }
        auto const group_it = std::ranges::find_if(
            connections.event_producer_groups,
            [&](EventProducerGroupPlan const& group) {
                // Groups also differ in their readers' compensation; the
                // connection's own group is the one that lists it.
                auto const connection_index = static_cast<std::size_t>(
                    &connection - connections.event_connections.data());
                return group.source_type == connection.source_type
                    && group.sources == connection.sources
                    && source_conversions_match(group, connection)
                    && std::ranges::contains(
                        group.connection_indices, connection_index);
            });
        if (group_it == connections.event_producer_groups.end()) {
            return std::unexpected(
                "GraphJit composed event detach lost its producer group");
        }
        auto const group_index = static_cast<std::size_t>(std::distance(
            connections.event_producer_groups.begin(), group_it));
        if (group_index >= plan.producer_group_representations.size()
            || !plan.producer_group_representations[group_index]) {
            return std::unexpected(
                "GraphJit composed event detach has no producer representation");
        }
        auto const canonical =
            *plan.producer_group_representations[group_index];
        if (canonical >= plan.ports.size()
            || !plan.ports[canonical].has_source_indices) {
            return std::unexpected(
                "GraphJit composed event detach requires ordered fan-in storage");
        }
        if (group_index
                >= plan.producer_group_source_representations.size()
            || plan.producer_group_source_representations[group_index].size()
                != connection.sources.size()) {
            return std::unexpected(
                "GraphJit composed event detach has no per-source producer representations");
        }
        for (auto const source_id : connection.sources) {
            if (source_id.bundle
                    >= connections.schedule.bundle_execution_position.size()
                || !connections.schedule.bundle_execution_position[
                    source_id.bundle]) {
                return std::unexpected(
                    "GraphJit composed event source has no schedule position");
            }
        }

        std::vector<std::optional<std::size_t>> delayed_sources(
            connection.source_plans.size());
        for (std::size_t source_index = 0;
             source_index < connection.source_plans.size();
             ++source_index) {
            auto const& source = connection.source_plans[source_index];
            if (!source.detach) continue;
            auto const source_representation =
                plan.producer_group_source_representations[
                    group_index][source_index];
            if (!source_representation) {
                return std::unexpected(
                    "GraphJit composed event detach lost its source representation");
            }
            auto const source_id = connection.sources[source_index];
            auto delayed = plan_source_event_feedback(
                connection,
                source,
                source_index,
                group_index,
                *source_representation,
                *connections.schedule.bundle_execution_position[
                    source_id.bundle]);
            if (!delayed)
                return std::unexpected(std::move(delayed.error()));
            delayed_sources[source_index] = *delayed;
        }

        auto const identity = EventConversionRegistry::plan(
            connection.source_type, connection.source_type);
        for (auto const target_id : connection.targets) {
            auto const target_primitive = primitive_index_for_bundle(
                target_id.bundle);
            if (!target_primitive
                || target_id.port
                    >= plan.primitives[*target_primitive].inputs.size()
                || target_id.bundle
                    >= connections.schedule.bundle_execution_position.size()
                || !connections.schedule.bundle_execution_position[
                    target_id.bundle]) {
                return std::unexpected(
                    "GraphJit composed event detach requires a scheduled concrete consumer");
            }
            NodeBundlePortId const target_port{
                target_id.bundle, PortKind::event, target_id.port};
            auto const target = graph.node_bundles
                .resolve_event_input(target_port).config;
            if (!is_sequential(target.access)
                || target.type != connection.target_type) {
                return std::unexpected(
                    "GraphJit composed event consumer disagrees with its declaration");
            }
            auto const scope = primitive_scope(
                *connections.schedule.bundle_execution_position[
                    target_id.bundle],
                EventOperationPhase::before);

            std::vector<std::size_t> branch_representations;
            branch_representations.reserve(connection.source_plans.size());
            std::size_t merged_capacity = 0;
            for (std::size_t source_index = 0;
                 source_index < connection.source_plans.size();
                 ++source_index) {
                auto const& source = connection.source_plans[source_index];
                auto const source_representation = delayed_sources[source_index]
                    .value_or(canonical);
                auto const source_filter = delayed_sources[source_index]
                    ? std::optional<std::size_t>{}
                    : std::optional<std::size_t>{source_index};
                if (source_representation >= plan.ports.size()) {
                    return std::unexpected(
                        "GraphJit composed event branch references invalid storage");
                }
                auto const branch_capacity =
                    plan.ports[source_representation].event_capacity;
                if (merged_capacity
                    > std::numeric_limits<std::size_t>::max()
                        - branch_capacity) {
                    return std::unexpected(
                        "GraphJit composed event capacity overflows size_t");
                }
                merged_capacity += branch_capacity;

                auto materialized_events = materialization_event_bound(
                    source.max_events_per_index,
                    connection.target_history,
                    scope,
                    true,
                    branch_capacity);
                if (!materialized_events) {
                    return std::unexpected(
                        std::move(materialized_events.error()));
                }
                auto branch = require_event_materialization(
                    plan, group_index, EventMaterializationPlan{
                        .source_representation = source_representation,
                        .conversion = identity,
                        .source_index_filter = source_filter,
                        .history_samples = connection.target_history,
                        .maximum_output_event_count =
                            materialized_events->output_writes,
                        .maximum_source_event_reads =
                            materialized_events->source_reads,
                        .select_invocation_window = true,
                        .before_merge = true,
                        .scope = scope,
                    });
                if (!branch) {
                    return std::unexpected(std::move(branch.error()));
                }
                branch_representations.push_back(*branch);
            }

            auto rounded_capacity = rounded_event_capacity(merged_capacity);
            if (!rounded_capacity)
                return std::unexpected(std::move(rounded_capacity.error()));
            auto merged = append_representation(
                group_index,
                connection.source_type,
                *rounded_capacity,
                false,
                false,
                {},
                false,
                true);
            if (!merged)
                return std::unexpected(std::move(merged.error()));
            EventMergePlan merge{
                .source_representations =
                    std::move(branch_representations),
                .target_representation = *merged,
                .scope = scope,
                .ordering_history_samples = connection.target_history,
            };
            merge.source_indices.resize(connection.source_plans.size());
            std::iota(
                merge.source_indices.begin(), merge.source_indices.end(), 0);
            plan.merges.push_back(std::move(merge));

            auto consumer_representation = *merged;
            if (connection.conversion.size() != 0) {
                auto materialized_events = materialization_event_bound(
                    connection.max_events_per_index,
                    connection.target_history,
                    scope,
                    true,
                    *rounded_capacity);
                if (!materialized_events) {
                    return std::unexpected(
                        std::move(materialized_events.error()));
                }
                auto converted = require_event_materialization(
                    plan, group_index, EventMaterializationPlan{
                        .source_representation = *merged,
                        .conversion = connection.conversion,
                        .history_samples = connection.target_history,
                        .maximum_output_event_count =
                            materialized_events->output_writes,
                        .maximum_source_event_reads =
                            materialized_events->source_reads,
                        .select_invocation_window = true,
                        .scope = scope,
                    });
                if (!converted) {
                    return std::unexpected(std::move(converted.error()));
                }
                consumer_representation = *converted;
            }
            auto& target_binding =
                plan.primitives[*target_primitive].inputs[target_id.port];
            if (target_binding.storage) {
                return std::unexpected(
                    "GraphJit composed event consumer input is connected more than once");
            }
            target_binding.storage = consumer_representation;
        }
    }

    // Complete disconnected realtime ports explicitly. Inputs supplied by
    // background evaluation bind their TickInvocationCall slot; disconnected
    // inputs bind the immutable empty sequence of their type. Outputs use the
    // same transient/carry/ring policy as a connected stream: disconnection
    // removes delivery, not an authored Tick output's own history/latency
    // state.
    for (std::size_t primitive_index = 0;
        primitive_index < plan.primitives.size(); ++primitive_index) {
        if (analysis.primitives[primitive_index].implementation->tick_block.empty()) {
            continue;
        }
        auto const bundle = analysis.primitives[primitive_index].bundle.node_bundle;

        for (std::size_t port = 0;
             port < plan.primitives[primitive_index].inputs.size(); ++port) {
            auto& binding = plan.primitives[primitive_index].inputs[port];
            if (binding.storage) continue;
            auto invocation_supplied = external_tick_sequential_slot(
                connections.background, bundle, PortKind::event, port);
            if (!invocation_supplied) {
                return std::unexpected(
                    std::move(invocation_supplied.error()));
            }
            if (*invocation_supplied) {
                binding.invocation_supplied = true;
                auto const& runtime_binding = connections.background
                    .tick_runtime.sequential_event_inputs[
                        **invocation_supplied];
                if (runtime_binding.port >= connections.background.ports.size()) {
                    return std::unexpected(
                        "GraphJit invocation-supplied event input lost its logical port");
                }
                auto const& logical =
                    connections.background.ports[runtime_binding.port];
                if (logical.sequential_history
                    > std::numeric_limits<std::size_t>::max()
                        - block_size) {
                    return std::unexpected(
                        "GraphJit invocation-supplied event window overflows size_t");
                }
                double maximum_rate = 0.0;
                for (auto const& read : runtime_binding.reads) {
                    auto const storage = read.storage;
                    if (storage >= connections.background.storage.ports.size()) {
                        return std::unexpected(
                            "GraphJit invocation-supplied event input references missing storage");
                    }
                    maximum_rate = std::max(
                        maximum_rate,
                        connections.background.storage.ports[storage]
                            .max_events_per_index);
                }
                // Without history the callback reads the slot's current
                // window directly.
                if (logical.sequential_history == 0) continue;
                auto const capacity = event_sequence_capacity_for_sample_span(
                    maximum_rate,
                    block_size
                        + logical.sequential_history);
                if (!capacity) {
                    return std::unexpected(
                        "GraphJit invocation-supplied event capacity is not representable");
                }
                auto timeline = append_input_timeline(
                    binding,
                    EventInputPortId{.bundle = bundle, .port = port},
                    logical.event_type,
                    logical.sequential_history,
                    *capacity);
                if (!timeline) {
                    return std::unexpected(std::move(timeline.error()));
                }
                continue;
            }
            NodeBundlePortId const port_id{bundle, PortKind::event, port};
            auto const config = graph.node_bundles
                .resolve_event_input(port_id).config;
            // See the Random Access case in plan_sample_ports().
            if (!is_sequential(config)) {
                binding.random_access = true;
                continue;
            }
            auto empty = immutable_empty_representation(config.type);
            if (!empty) return std::unexpected(std::move(empty.error()));
            binding.storage = *empty;
        }

        for (std::size_t port = 0;
             port < plan.primitives[primitive_index].outputs.size(); ++port) {
            auto& binding = plan.primitives[primitive_index].outputs[port];
            if (!binding.realtime) continue;
            if (binding.storage) continue;
            auto authored = ensure_raw_event_output(EventOutputPortId{
                .bundle = bundle,
                .port = port,
            });
            if (!authored) {
                return std::unexpected(std::move(authored.error()));
            }
        }
    }

    auto storage = realize_event_storage(
        plan, std::move(group_cost_requirements), group_requires_retention,
        group_sequences, feedback_sequences, cost_model);
    if (!storage) return std::unexpected(std::move(storage.error()));
    return plan;
}

std::expected<void, std::string> layout_event_ports(
    EventPortBindingPlan& plan, GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections)
{
    for (auto& port : plan.ports) {
        auto layout = layout_event_sequence(port);
        if (!layout) return std::unexpected(std::move(layout.error()));
    }

    // Derive buffer lifetimes from the operations that actually access
    // each event sequence. Producer-group intervals are intentionally not used
    // here: a conversion result, merge input, or feedback working buffer often
    // exists for only a small suffix of the producer group's full interval.
    std::vector<std::optional<ConnectionLiveIntervalPlan>> buffer_lifetimes(
        plan.ports.size());

    std::vector<std::size_t> primitive_execution_positions(
        analysis.primitives.size());
    for (std::size_t primitive_index = 0;
         primitive_index < analysis.primitives.size(); ++primitive_index) {
        if (analysis.primitives[primitive_index].implementation->tick_block.empty()) {
            continue;
        }
        auto const bundle = analysis.primitives[primitive_index].bundle.node_bundle;
        // A callback-elided producer reads and writes no event buffer.
        if (bundle < connections.schedule.callback_elided.size()
            && connections.schedule.callback_elided[bundle] != 0) {
            continue;
        }
        if (bundle >= connections.schedule.bundle_execution_position.size()
            || !connections.schedule.bundle_execution_position[bundle]) {
            return std::unexpected(
                "GraphJit event stack lifetime planning lost a primitive schedule position");
        }
        primitive_execution_positions[primitive_index] =
            *connections.schedule.bundle_execution_position[bundle];
    }

    struct EventRegionBounds {
        bool cyclic = false;
        std::size_t begin = 0;
        std::size_t end = 0;
    };
    std::vector<EventRegionBounds> region_bounds(
        connections.schedule.regions.size());
    std::vector<std::optional<std::size_t>> execution_position_region(
        analysis.primitives.size());
    for (std::size_t region_index = 0;
         region_index < connections.schedule.regions.size(); ++region_index) {
        auto const& region = connections.schedule.regions[region_index];
        if (region.execution_order.empty()) continue;
        auto begin = std::numeric_limits<std::size_t>::max();
        std::size_t end = 0;
        for (auto const bundle : region.execution_order) {
            if (bundle >= connections.schedule.bundle_execution_position.size()
                || !connections.schedule.bundle_execution_position[bundle]) {
                return std::unexpected(
                    "GraphJit event stack lifetime planning lost a region schedule position");
            }
            auto const position =
                *connections.schedule.bundle_execution_position[bundle];
            begin = std::min(begin, position);
            end = std::max(end, position);
            if (position >= execution_position_region.size()) {
                return std::unexpected(
                    "GraphJit event stack lifetime planning found an invalid execution position");
            }
            execution_position_region[position] = region_index;
        }
        region_bounds[region_index] = EventRegionBounds{
            .cyclic = region.cyclic,
            .begin = begin,
            .end = end,
        };
    }

    auto touch_buffer = [&](std::size_t representation_index,
                                    std::size_t begin,
                                    std::size_t end)
        -> std::expected<void, std::string> {
        if (representation_index >= plan.ports.size()) {
            return std::unexpected(
                "GraphJit event stack lifetime references an invalid buffer");
        }
        if (begin > end) {
            return std::unexpected(
                "GraphJit event stack lifetime has an inverted schedule interval");
        }
        auto& live = buffer_lifetimes[representation_index];
        if (!live) {
            live = ConnectionLiveIntervalPlan{
                .begin = begin,
                .end = end,
                .crosses_kernel_invocations = false,
            };
        } else {
            live->begin = std::min(live->begin, begin);
            live->end = std::max(live->end, end);
        }
        return {};
    };
    auto touch_at = [&](std::size_t representation_index,
                        std::size_t position)
        -> std::expected<void, std::string> {
        return touch_buffer(
            representation_index, position, position);
    };
    auto scope_position = [&](EventOperationScope scope)
        -> std::expected<std::size_t, std::string> {
        if (scope.kind == EventOperationScopeKind::primitive) {
            if (scope.index >= analysis.primitives.size()) {
                return std::unexpected(
                    "GraphJit event operation has an invalid primitive scope");
            }
            return scope.index;
        }
        if (scope.index >= region_bounds.size()
            || connections.schedule.regions[scope.index].execution_order.empty()) {
            return std::unexpected(
                "GraphJit event operation has an invalid region scope");
        }
        auto const& bounds = region_bounds[scope.index];
        return scope.phase == EventOperationPhase::before
            ? bounds.begin
            : bounds.end;
    };

    // Primitive callbacks are the direct writers/readers of event buffers.
    for (std::size_t primitive_index = 0;
         primitive_index < plan.primitives.size(); ++primitive_index) {
        auto const position = primitive_execution_positions[primitive_index];
        for (auto const& output : plan.primitives[primitive_index].outputs) {
            if (!output.storage) continue;
            auto begin = position;
            auto end = position;
            // An append-existing output in an SCC accumulates one sequence
            // across every slice of the root invocation. Keep that buffer live
            // across the whole SCC; ordinary per-slice materialization buffers
            // remain eligible for tighter reuse.
            if (output.append_existing
                && position < execution_position_region.size()
                && execution_position_region[position]) {
                auto const& bounds =
                    region_bounds[*execution_position_region[position]];
                if (bounds.cyclic) {
                    begin = bounds.begin;
                    end = bounds.end;
                }
            }
            if (auto touched = touch_buffer(
                    *output.storage, begin, end);
                !touched) {
                return std::unexpected(std::move(touched.error()));
            }
        }
        for (auto const& input_binding : plan.primitives[primitive_index].inputs) {
            if (input_binding.storage) {
                if (auto touched = touch_at(*input_binding.storage, position);
                    !touched) {
                    return std::unexpected(std::move(touched.error()));
                }
            }
            for (auto const operand :
                 {input_binding.realtime_operand_storage,
                  input_binding.combined_storage}) {
                if (!operand) continue;
                if (auto touched = touch_at(*operand, position); !touched) {
                    return std::unexpected(std::move(touched.error()));
                }
            }
        }
    }

    // Restore happens before the producer step (or before the complete cyclic
    // region); commit reads the same working buffer after the producer step (or
    // after the complete cyclic region).
    for (auto const& carry : plan.carry_operations) {
        auto const begin = scope_position(carry.restore_scope);
        auto const end = scope_position(carry.commit_scope);
        if (!begin) return std::unexpected(std::move(begin.error()));
        if (!end) return std::unexpected(std::move(end.error()));
        if (auto touched = touch_buffer(
                carry.working_representation,
                std::min(*begin, *end),
                std::max(*begin, *end));
            !touched) {
            return std::unexpected(std::move(touched.error()));
        }
        if (auto touched = touch_buffer(carry.persistent_representation, std::min(*begin, *end), std::max(*begin, *end));
            !touched) return std::unexpected(std::move(touched.error()));
    }

    for (auto const& ring : plan.persistent_rings) {
        auto const position = scope_position(ring.prune_scope);
        if (!position) return std::unexpected(std::move(position.error()));
        if (auto touched = touch_at(ring.storage, *position); !touched)
            return std::unexpected(std::move(touched.error()));
    }

    for (auto const& reset : plan.sequence_resets) {
        if (reset.storage >= plan.ports.size())
            return std::unexpected("GraphJit event sequence reset references an invalid buffer");
        if (!plan.ports[reset.storage].persistent) continue;
        auto const position = scope_position(reset.scope);
        if (!position) return std::unexpected(std::move(position.error()));
        if (auto touched = touch_at(reset.storage, *position); !touched)
            return std::unexpected(std::move(touched.error()));
    }

    // A merge reads every source and writes/extends the target after the last
    // producer. Earlier producer-local buffers therefore end exactly at the
    // merge step instead of at the producer group's last consumer.
    for (auto const& merge : plan.merges) {
        auto const operation_position = scope_position(merge.scope);
        if (!operation_position) {
            return std::unexpected(std::move(operation_position.error()));
        }
        for (auto const source_representation : merge.source_representations) {
            if (auto touched = touch_at(
                    source_representation, *operation_position);
                !touched) {
                return std::unexpected(std::move(touched.error()));
            }
        }
        auto target_begin = *operation_position;
        auto target_end = *operation_position;
        if (merge.preserve_existing_target
            && merge.scope.kind == EventOperationScopeKind::primitive
            && *operation_position < execution_position_region.size()
            && execution_position_region[*operation_position]) {
            auto const& bounds = region_bounds[
                *execution_position_region[*operation_position]];
            if (bounds.cyclic) {
                // The merge extends this sequence once per SCC slice. A
                // linear interval beginning at the merge position does not
                // cover the earlier primitive positions on the next loop
                // iteration, so another transient allocation could overwrite
                // the accumulated prefix before the following merge.
                target_begin = bounds.begin;
                target_end = bounds.end;
            }
        }
        if (auto touched = touch_buffer(
                merge.target_representation, target_begin, target_end);
            !touched) {
            return std::unexpected(std::move(touched.error()));
        }
    }

    // Scope is part of the storage operation. Lifetime planning therefore
    // consumes the selected slice/root-call boundary directly and never has to
    // reverse-engineer it from storage consumers.
    for (auto const& materialization : plan.materializations) {
        auto const operation_position = scope_position(materialization.scope);
        if (!operation_position) {
            return std::unexpected(std::move(operation_position.error()));
        }
        if (auto touched = touch_at(
                materialization.source_representation, *operation_position);
            !touched) {
            return std::unexpected(std::move(touched.error()));
        }
        if (auto touched = touch_at(
                materialization.target_representation, *operation_position);
            !touched) {
            return std::unexpected(std::move(touched.error()));
        }
    }

    // Feedback appends read the source and write the delayed buffer after the
    // producer. The target is then consumed on a later SCC slice, so a stack
    // buffer written inside an SCC crosses the loop back-edge. A linear
    // first/last schedule interval cannot express that wrapped lifetime; keep
    // the target allocated for the complete SCC invocation. Retained feedback
    // buffers are already widened by their restore/commit operations above.
    for (auto const& feedback : plan.feedback_operations) {
        auto const operation_position = scope_position(feedback.append_scope);
        if (!operation_position) {
            return std::unexpected(std::move(operation_position.error()));
        }
        if (auto touched = touch_at(
                feedback.source_representation,
                *operation_position);
            !touched) {
            return std::unexpected(std::move(touched.error()));
        }

        auto target_begin = *operation_position;
        auto target_end = *operation_position;
        if (feedback.append_scope.kind == EventOperationScopeKind::region) {
            if (feedback.append_scope.index >= region_bounds.size()) {
                return std::unexpected(
                    "GraphJit event feedback has an invalid region scope");
            }
            auto const& bounds = region_bounds[feedback.append_scope.index];
            if (bounds.cyclic) {
                target_begin = bounds.begin;
                target_end = bounds.end;
            }
        } else if (*operation_position < execution_position_region.size()
                   && execution_position_region[*operation_position]) {
            auto const& bounds = region_bounds[
                *execution_position_region[*operation_position]];
            if (bounds.cyclic) {
                target_begin = bounds.begin;
                target_end = bounds.end;
            }
        }
        if (auto touched = touch_buffer(
                feedback.target_representation,
                target_begin,
                target_end);
            !touched) {
            return std::unexpected(std::move(touched.error()));
        }
    }

    std::vector<TransientArenaAllocationRequest> transient_requests;
    std::vector<std::size_t> transient_representations;
    transient_requests.reserve(plan.ports.size());
    transient_representations.reserve(plan.ports.size());
    for (std::size_t representation_index = 0;
         representation_index < plan.ports.size();
         ++representation_index) {
        auto const& storage = plan.ports[representation_index];
        auto const& lifetime = buffer_lifetimes[representation_index];
        if (storage.persistent) {
            auto const boundary = connections.schedule.early_execution_steps;
            plan.persistent_storage_crosses_tick_partition |= lifetime
                && lifetime->begin < boundary && lifetime->end >= boundary;
            continue;
        }
        if (storage.immutable_empty) continue;
        if (!lifetime) {
            return std::unexpected(
                "GraphJit transient event buffer has no scheduled reader or writer");
        }
        transient_requests.push_back(TransientArenaAllocationRequest{
            .size_bytes = storage.size_bytes,
            .alignment = storage.alignment,
            .live_interval = *lifetime,
        });
        transient_representations.push_back(representation_index);
    }
    auto arena = plan_transient_arena(transient_requests);
    if (!arena) return std::unexpected(std::move(arena.error()));
    plan.transient_arena_size = arena->size_bytes;
    plan.transient_arena_alignment = arena->alignment;
    plan.transient_allocations.reserve(arena->allocations.size());
    for (std::size_t request_index = 0;
         request_index < arena->allocations.size();
         ++request_index) {
        auto const representation_index = transient_representations[request_index];
        auto const& allocation = arena->allocations[request_index];
        auto const allocation_index = plan.transient_allocations.size();
        plan.transient_allocations.push_back(EventTransientAllocationPlan{
            .representation_index = representation_index,
            .size_bytes = allocation.size_bytes,
            .alignment = allocation.alignment,
            .region_relative_offset = allocation.offset,
            .live_interval = transient_requests[request_index].live_interval,
        });
        plan.ports[representation_index].transient_allocation =
            allocation_index;
    }

    return {};
}

} // namespace

std::expected<EventPortBindingPlan, std::string> plan_event_ports(
    ConfiguredGraph const& graph,
    std::size_t block_size,
    GraphAnalysis const& analysis,
    ConnectionAnalysisPlan const& connections,
    ResolvedPortRealization const& realization,
    RealtimeStorageCostModel const& cost_model)
{
    if (analysis.empty) return EventPortBindingPlan{};
    auto const bundle_count = graph.node_bundles.size();
    EventPortInventory inventory{
        .primitives = std::vector<std::optional<std::size_t>>(bundle_count),
        .nodes = std::vector<PlannedGraphNode const*>(bundle_count),
    };
    for (std::size_t index = 0; index < analysis.primitives.size(); ++index) {
        auto const bundle = analysis.primitives[index].bundle.node_bundle;
        if (bundle >= bundle_count) return std::unexpected("GraphJit event planning found an invalid primitive bundle");
        inventory.primitives[bundle] = index;
    }
    for (auto const& node : connections.nodes) {
        if (node.bundle >= bundle_count) return std::unexpected("GraphJit event planning found an invalid planned bundle");
        inventory.nodes[node.bundle] = &node;
    }
    auto bound = bind_event_ports(graph, block_size, analysis, connections, realization, cost_model, inventory);
    if (!bound) return std::unexpected(std::move(bound.error()));
    auto plan = std::move(*bound);
    auto layout = layout_event_ports(plan, analysis, connections);
    if (!layout) return std::unexpected(std::move(layout.error()));
    return plan;
}

} // namespace iv::graph_jit::detail
