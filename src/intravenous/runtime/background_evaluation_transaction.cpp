#include <intravenous/runtime/background_evaluation_transaction.h>

#include <intravenous/runtime/background_evaluation_call_frame.h>
#include <intravenous/runtime/background_storage_realization.h>
#include <intravenous/runtime/realtime_produced_record.h>
#include <intravenous/graph_jit/background_storage_plan.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace iv {
namespace {

[[nodiscard]] bool event_matches_type(
    EventTypeId type, Event const& event) noexcept
{
    switch (type) {
    case EventTypeId::midi:
        return std::holds_alternative<MidiEvent>(event);
    case EventTypeId::trigger:
        return std::holds_alternative<TriggerEvent>(event);
    case EventTypeId::boundary:
        return std::holds_alternative<BoundaryEvent>(event);
    case EventTypeId::sample:
        return std::holds_alternative<SampleEvent>(event);
    case EventTypeId::curve_point:
        return std::holds_alternative<CurvePointEvent>(event);
    case EventTypeId::empty:
        return std::holds_alternative<EmptyEvent>(event);
    case EventTypeId::count:
        return false;
    }
    return false;
}

[[nodiscard]] IndexCoverage shifted_back(IndexCoverage const& coverage,
                                    std::size_t latency)
{
    IndexCoverage result;
    for (auto const region : coverage) {
        result.include({
            .begin = region.begin.retreated_by(latency),
            .extent = region.extent,
        });
    }
    return result;
}

[[nodiscard]] bool
runtime_produced(graph_jit::BackgroundEvaluationPlan const& plan,
                 graph_jit::PortStoragePlan const& storage) noexcept
{
    if (storage.sample_materialization || storage.event_materialization)
        return true;
    if (!storage.output_port || *storage.output_port >= plan.ports.size())
        return false;
    auto const& output = plan.ports[*storage.output_port];
    return output.authored_tock_output || output.replayed_tick_output;
}

[[nodiscard]] std::vector<GlobalIndexRange> replay_regions(IndexCoverage const& coverage,
                                                      std::size_t maximum)
{
    if (maximum == 0 && !coverage.empty()) {
        throw std::invalid_argument(
            "background replay has no compiled block limit");
    }
    std::vector<GlobalIndexRange> result;
    for (auto const region : coverage) {
        GlobalIndex::extent_type consumed = 0;
        while (consumed < region.extent) {
            auto const extent = std::min<GlobalIndex::extent_type>(
                region.extent - consumed, maximum);
            result.push_back({
                .begin = region.begin.advanced_by(consumed),
                .extent = extent,
            });
            consumed += extent;
        }
    }
    return result;
}

} // namespace

class BackgroundEvaluationTransaction::Impl {
    struct DataMutation {
        PersistedOutputHandle output{};
        PortKind kind = PortKind::sample;
        ChannelLayout sample_layout{};
        EventTypeId event_type = EventTypeId::empty;
        IndexCoverage coverage{};
    };

    struct ProducedRecord {
        BackgroundProducedInputRoute route{};
        RealtimeProducedRecordHeader header{};
        std::vector<std::byte> payload{};
    };

    CompiledGraph const* graph_ = nullptr;
    TockStateStorage* tock_states_ = nullptr;
    BackgroundCoverageState* coverage_ = nullptr;
    BackgroundPropagationWorkspace* propagation_ = nullptr;
    PersistedDataStore* data_ = nullptr;
    TickMaterializationStore* materializations_ = nullptr;
    BackgroundTransactionInputs inputs_{};
    std::optional<std::size_t> remaining_materialized_sample_bytes_{};
    std::uint64_t semantic_version_ = 0;
    std::span<BackgroundProducedInputRoute const> produced_routes_{};
    std::span<PinnedBlockPrefix const> produced_prefixes_{};
    PersistedDataStore::ReaderSlot reader_{};
    std::optional<PersistedDataStore::ReaderPin> pin_{};
    std::optional<PreparedCoveragePropagation> prepared_{};
    // The published Tick materialization of this generation, if any, and the
    // part of each demanded Tick Sequential input that it still represents.
    TickMaterializationSnapshot const* previous_materialization_ = nullptr;
    std::vector<TickMaterializationRetention> materialization_retention_{};
    std::vector<DataMutation> data_mutations_{};
    std::vector<std::pair<PersistedOutputHandle, IndexCoverage>>
        captured_coverage_{};
    PersistedDataStore::Snapshot const* working_data_ = nullptr;
    PersistedDataStore::Candidate* active_candidate_ = nullptr;
    std::vector<PersistedPayloadSelection> prepared_payload_{};
    bool executed_ = false;

    void discard_prepared() noexcept
    {
        if (prepared_) {
            propagation_->discard(std::move(*prepared_));
            prepared_.reset();
        }
    }

    void release_reader() noexcept
    {
        active_candidate_ = nullptr;
        pin_.reset();
        reader_ = {};
    }

    [[nodiscard]] bool
    node_evaluates(graph_jit::BackgroundNodeIndex node) const noexcept
    {
        auto const activity = prepared_->node_activity();
        return node < activity.size() &&
               graph_jit::has_activity(
                   activity[node], graph_jit::BackgroundNodeActivity::evaluate);
    }

    void add_data_mutation(
        PersistedOutputHandle output,
        graph_jit::BackgroundPortPlan const& port,
        IndexCoverage coverage)
    {
        auto const captured = std::ranges::find_if(
            captured_coverage_, [&](auto const& entry) {
                return entry.first == output;
            });
        if (captured != captured_coverage_.end()) {
            coverage.exclude(captured->second);
        }
        if (coverage.empty()) return;
        auto found = std::ranges::find(
            data_mutations_, output, &DataMutation::output);
        if (found == data_mutations_.end()) {
            data_mutations_.push_back({
                .output = output,
                .kind = port.kind,
                .sample_layout = port.sample_layout,
                .event_type = port.event_type,
                .coverage = std::move(coverage),
            });
        } else {
            found->coverage.include(coverage);
        }
    }

    [[nodiscard]] std::expected<std::vector<ProducedRecord>, std::string>
    decode_produced_records() const
    {
        if (produced_routes_.size() != produced_prefixes_.size()) {
            return std::unexpected(
                "background produced-input routes and prefixes are not aligned");
        }

        std::vector<ProducedRecord> records;
        auto const& plan = graph_->background_evaluation_plan;
        for (std::size_t input = 0; input < produced_routes_.size(); ++input) {
            auto const& route = produced_routes_[input];
            auto const& prefix = produced_prefixes_[input];
            if (route.port >= plan.ports.size()) {
                return std::unexpected(
                    "background produced input references a missing port");
            }
            auto const& port = plan.ports[route.port];
            if (!route.output.valid()
                || port.direction != graph_jit::PortDirection::output
                || port.kind != route.kind || !port.persisted_tick_output
                || port.realization != OutputRealization::buffered
                || persisted_output_kind(route.output) != route.kind) {
                return std::unexpected(
                    "background produced input has an incompatible persisted route");
            }

            bool canonical_route = false;
            for (graph_jit::PortStorageIndex storage = 0;
                 storage < plan.storage.ports.size(); ++storage) {
                auto const& planned = plan.storage.ports[storage];
                if (planned.storage
                        != graph_jit::PortStorageKind::persisted_data
                    || planned.output_port != route.port
                    || planned.kind != route.kind) {
                    continue;
                }
                if (data_->resolve_output(persisted_output_id(
                        plan, storage, graph_->project_generation))
                    == route.output) {
                    canonical_route = true;
                    break;
                }
            }
            if (!canonical_route) {
                return std::unexpected(
                    "background produced input has no canonical persisted storage");
            }

            std::optional<ProducedRecord> current;
            std::array<std::byte, sizeof(RealtimeProducedRecordHeader)> header_bytes{};
            std::size_t header_size = 0;
            prefix.for_each([&](AsyncQueueBlock const& block) {
                auto bytes = block.used_storage();
                while (!bytes.empty()) {
                    if (!current) {
                        auto const size = std::min(bytes.size(), header_bytes.size() - header_size);
                        std::memcpy(header_bytes.data() + header_size, bytes.data(), size);
                        bytes = bytes.subspan(size);
                        header_size += size;
                        if (header_size != header_bytes.size()) break;
                        current.emplace(ProducedRecord{.route = route});
                        std::memcpy(&current->header, header_bytes.data(), header_bytes.size());
                        header_size = 0;
                    }
                    auto const size = std::min(bytes.size(), current->header.payload_size - current->payload.size());
                    auto const payload = bytes.first(size);
                    current->payload.insert(current->payload.end(), payload.begin(), payload.end());
                    bytes = bytes.subspan(size);
                    if (current->payload.size() == current->header.payload_size) {
                        records.push_back(std::move(*current));
                        current.reset();
                    }
                }
            });
            if (header_size != 0 || current) {
                return std::unexpected(
                    "realtime-produced record is incomplete");
            }
        }
        return records;
    }

    [[nodiscard]] std::expected<void, std::string> apply_sample_record(
        ProducedRecord const& record,
        PersistedDataStore::Candidate& candidate)
    {
        auto const& header = record.header;
        auto const& port =
            graph_->background_evaluation_plan.ports[record.route.port];
        auto const channels = channel_count(header.continuous_layout);
        if (header.payload_kind
                != RealtimeProducedPayloadKind::continuous_values
            || record.route.kind != PortKind::sample
            || header.continuous_layout != port.sample_layout
            || header.continuous_geometry != port.continuous_geometry
            || header.continuous_geometry.value_size_bytes == 0
            || channels == 0
            || header.range_extent
                > std::numeric_limits<std::size_t>::max()
            || static_cast<std::size_t>(header.range_extent)
                > std::numeric_limits<std::size_t>::max() / channels
            || static_cast<std::size_t>(header.range_extent) * channels
                > std::numeric_limits<std::size_t>::max()
                    / header.continuous_geometry.value_size_bytes
            || header.payload_size
                != static_cast<std::size_t>(header.range_extent) * channels
                    * header.continuous_geometry.value_size_bytes
            || header.range_extent == 0) {
            return std::unexpected(
                "realtime-produced continuous record has invalid metadata");
        }
        PersistedContinuousStorage values{
            header.payload_size,
            header.continuous_geometry.element_alignment};
        std::memcpy(
            values.data(), record.payload.data(), header.payload_size);
        PersistedSampleRegion retained{
            .output = record.route.output,
            .range = {
                .begin = header.begin,
                .extent = header.range_extent,
            },
            .layout = header.continuous_layout,
            .geometry = header.continuous_geometry,
            .continuous_values = std::move(values),
        };
        candidate.overwrite(std::move(retained));
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> apply_event_record(
        ProducedRecord const& record,
        PersistedDataStore::Candidate& candidate)
    {
        static_assert(std::is_trivially_copyable_v<TimedEvent>);
        auto const& header = record.header;
        auto const& port =
            graph_->background_evaluation_plan.ports[record.route.port];
        if (header.payload_kind != RealtimeProducedPayloadKind::events
            || record.route.kind != PortKind::event
            || header.event_type != port.event_type
            || header.event_count
                > std::numeric_limits<std::size_t>::max() / sizeof(TimedEvent)
            || header.payload_size != header.event_count * sizeof(TimedEvent)
            || header.range_extent == 0) {
            return std::unexpected(
                "realtime-produced event record has invalid metadata");
        }
        auto const range = GlobalIndexRange{
            .begin = header.begin,
            .extent = header.range_extent,
        };
        std::vector<TimedEvent> captured(header.event_count);
        if (!captured.empty()) {
            std::memcpy(
                captured.data(), record.payload.data(), header.payload_size);
        }
        GlobalIndex::extent_type previous = 0;
        bool first = true;
        for (auto const& event : captured) {
            auto const offset = range.offset_of(event.time);
            if (!offset || !event_matches_type(header.event_type, event.value)
                || (!first && *offset < previous)) {
                return std::unexpected(
                    "realtime-produced event payload is invalid");
            }
            previous = *offset;
            first = false;
        }
        candidate.overwrite(PersistedEventChunk{
            .output = record.route.output,
            .range = range,
            .type = header.event_type,
            .events = std::move(captured),
        });
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> apply_sample_void(
        ProducedRecord const& record,
        PersistedDataStore::Candidate& candidate)
    {
        auto const& header = record.header;
        auto const& port =
            graph_->background_evaluation_plan.ports[record.route.port];
        if (header.payload_kind != RealtimeProducedPayloadKind::void_value
            || record.route.kind != PortKind::sample
            || header.continuous_layout != port.sample_layout
            || header.continuous_geometry != port.continuous_geometry
            || header.payload_size != 0 || !record.payload.empty()
            || header.range_extent == 0) {
            return std::unexpected(
                "realtime-produced continuous void has invalid metadata");
        }
        candidate.erase_sample(
            record.route.output,
            GlobalIndexRange{
                .begin = header.begin,
                .extent = header.range_extent,
            },
            header.continuous_layout);
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> apply_event_void(
        ProducedRecord const& record,
        PersistedDataStore::Candidate& candidate)
    {
        auto const& header = record.header;
        auto const& port =
            graph_->background_evaluation_plan.ports[record.route.port];
        if (header.payload_kind != RealtimeProducedPayloadKind::void_value
            || record.route.kind != PortKind::event
            || header.event_type != port.event_type
            || header.payload_size != 0 || !record.payload.empty()
            || header.event_count != 0 || header.range_extent == 0) {
            return std::unexpected(
                "realtime-produced event void has invalid metadata");
        }
        candidate.erase_events(
            record.route.output,
            GlobalIndexRange{
                .begin = header.begin,
                .extent = header.range_extent,
            },
            header.event_type);
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> apply_produced_records(
        std::span<ProducedRecord const> records,
        PersistedDataStore::Snapshot const& base,
        PersistedDataStore::Candidate& candidate)
    {
        struct Root {
            BackgroundProducedInputRoute route{};
            IndexCoverage coverage{};
            IndexCoverage changed{};
        };
        std::vector<Root> roots;
        for (auto const& record : records) {
            if (record.header.range_extent == 0) {
                return std::unexpected(
                    "realtime-produced record has an invalid timeline window");
            }
            auto const changed = GlobalIndexRange{
                .begin = record.header.begin,
                .extent = record.header.range_extent,
            };
            auto found = std::ranges::find_if(roots, [&](Root const& root) {
                return root.route.port == record.route.port
                    && root.route.output == record.route.output;
            });
            if (found == roots.end()) {
                IndexCoverage coverage;
                if (record.route.kind == PortKind::sample) {
                    auto const& port = graph_->background_evaluation_plan
                        .ports[record.route.port];
                    if (auto const* existing =
                            base.find_sample_coverage(
                                record.route.output,
                                port.sample_layout,
                                port.continuous_geometry)) {
                        coverage = *existing;
                    }
                } else {
                    auto const type = graph_->background_evaluation_plan
                        .ports[record.route.port].event_type;
                    if (auto const* existing =
                            base.find_event_coverage(record.route.output, type)) {
                        coverage = *existing;
                    }
                }
                roots.push_back({
                    .route = record.route,
                    .coverage = std::move(coverage),
                });
                found = std::prev(roots.end());
            }
            auto const is_void = record.header.payload_kind
                == RealtimeProducedPayloadKind::void_value;
            if (is_void) {
                auto removed = found->coverage & IndexCoverage{changed};
                found->coverage.exclude(changed);
                found->changed.include(removed);
            } else {
                found->coverage.include(changed);
                found->changed.include(changed);
            }

            auto applied = is_void
                ? (record.route.kind == PortKind::sample
                    ? apply_sample_void(record, candidate)
                    : apply_event_void(record, candidate))
                : (record.route.kind == PortKind::sample
                    ? apply_sample_record(record, candidate)
                    : apply_event_record(record, candidate));
            if (!applied) return applied;

            auto captured = std::ranges::find_if(
                captured_coverage_, [&](auto const& entry) {
                    return entry.first == record.route.output;
                });
            if (captured == captured_coverage_.end()) {
                captured_coverage_.push_back({
                    record.route.output,
                    IndexCoverage{changed},
                });
            } else {
                captured->second.include(changed);
            }
        }

        for (auto& root : roots) {
            auto existing = std::ranges::find_if(
                inputs_.roots.output_changes,
                [&](OutputCoverageChangeRequest const& change) {
                    return change.port == root.route.port;
                });
            if (existing == inputs_.roots.output_changes.end()) {
                inputs_.roots.output_changes.push_back({
                    .port = root.route.port,
                    .coverage = std::move(root.coverage),
                    .changed = std::move(root.changed),
                });
            } else {
                existing->coverage = std::move(root.coverage);
                existing->changed.include(root.changed);
            }
        }
        return {};
    }

    static bool cached_output(void* opaque, graph_jit::BackgroundPortIndex port)
    {
        auto& self = *static_cast<Impl*>(opaque);
        auto const& plan = self.graph_->background_evaluation_plan;
        return std::ranges::any_of(plan.storage.ports, [&](auto const& slot) {
            return slot.output_port == port && runtime_produced(plan, slot)
                && uses_output_cache(plan, slot, bool(self.inputs_.reconstruction_graph));
        });
    }

    static void
    expand_output_demand(void* opaque, graph_jit::BackgroundPortIndex port,
                         IndexCoverage const& available,
                         std::span<GlobalIndexRange const>,
                         IndexCoverage const& changed,
                         IndexCoverage const& required, IndexCoverage& additional)
    {
        auto& self = *static_cast<Impl*>(opaque);
        auto const& plan = self.graph_->background_evaluation_plan;
        auto const& snapshot = *self.working_data_;
        // A change invalidates cached values; it does not demand recomputing
        // the whole logical Region. Only the consumer's requested positions do.
        auto const affected = required;
        if (affected.empty() && changed.empty()) return;

        for (graph_jit::PortStorageIndex index = 0;
             index < plan.storage.ports.size(); ++index) {
            auto const& storage = plan.storage.ports[index];
            if (!uses_output_cache(plan, storage, bool(self.inputs_.reconstruction_graph)) ||
                storage.output_port != port ||
                !runtime_produced(plan, storage)) {
                continue;
            }
            auto const output = self.data_->resolve_output(persisted_output_id(
                plan, index, self.graph_->project_generation));
            IndexCoverage existing;
            if (storage.kind == PortKind::sample) {
                if (auto const* retained = snapshot.find_sample_coverage(
                        output,
                        storage.sample_layout,
                        storage.continuous_geometry)) {
                    existing = *retained;
                }
            } else if (auto const* retained = snapshot.find_event_coverage(
                           output, storage.event_type)) {
                existing = *retained;
            }

            auto desired = available & affected;
            additional.include(desired - (existing - changed));

            auto invalidated = existing & changed;
            if (!invalidated.empty()) {
                self.add_data_mutation(
                    output, plan.ports[port], std::move(invalidated));
            }
        }
    }

    // Coverage of a Tick Sequential input that the forward traversal changed.
    // Its materialized values derive from the routed source outputs, each
    // shifted forward by its read latency.
    [[nodiscard]] IndexCoverage changed_tick_input_coverage(
        graph_jit::BackgroundPortIndex input,
        BackgroundForwardOutputChanges const& forward) const
    {
        auto const& plan = graph_->background_evaluation_plan;
        IndexCoverage changed;
        for (auto const& connection : plan.connections) {
            if (connection.kind == PortKind::sample) {
                if (connection.sample_target_port != input) continue;
                for (std::size_t channel = 0;
                     channel < connection.sample_source_port_by_channel.size();
                     ++channel) {
                    auto const source =
                        connection.sample_source_port_by_channel[channel];
                    if (!source) continue;
                    auto const latency =
                        channel < connection.sample_read_latencies.size()
                        ? connection.sample_read_latencies[channel]
                        : std::size_t{0};
                    for (auto const region : forward.changed(*source)) {
                        changed.include({
                            .begin = region.begin.advanced_by(latency),
                            .extent = region.extent,
                        });
                    }
                }
                continue;
            }
            for (auto const& delivery : connection.event_deliveries) {
                if (delivery.target_port != input || !delivery.source_port) {
                    continue;
                }
                changed.include(forward.changed(*delivery.source_port));
            }
        }
        return changed;
    }

    // Removes from a Tick playback demand the coverage that the previous
    // snapshot still represents. Other input demands, such as external
    // Random Access fetches, are never restricted.
    static void restrict_input_demand(
        void* opaque,
        graph_jit::BackgroundPortIndex port,
        BackgroundForwardOutputChanges const& forward,
        IndexCoverage& required)
    {
        auto& self = *static_cast<Impl*>(opaque);
        auto const& plan = self.graph_->background_evaluation_plan;
        if (!self.previous_materialization_
            || !std::ranges::contains(plan.tick_sequential_inputs, port)) {
            return;
        }
        auto const changed = self.changed_tick_input_coverage(port, forward);
        IndexCoverage retained;
        if (auto const* sample = self.previous_materialization_->find_sample(port)) {
            for (auto const& region : sample->regions) {
                retained.include((IndexCoverage{region.range} & required) - changed);
            }
        } else if (auto const* event =
                       self.previous_materialization_->find_event(port)) {
            for (auto const& region : event->chunks) {
                auto const valid = region.prepared_coverage
                    ? *region.prepared_coverage : IndexCoverage{region.range};
                retained.include((valid & required) - changed);
            }
        }
        if (retained.empty()) return;
        required = required - retained;
        auto existing = std::ranges::find(
            self.materialization_retention_, port,
            &TickMaterializationRetention::port);
        if (existing == self.materialization_retention_.end()) {
            self.materialization_retention_.push_back({
                .port = port,
                .retained = std::move(retained),
                .input_access = plan.ports[port].input_access,
            });
        } else {
            existing->retained.include(retained);
        }
    }

    [[nodiscard]] BackgroundEvaluationCallFrameSelection
    binding_selection() const
    {
        auto const& plan = graph_->background_evaluation_plan;
        BackgroundEvaluationCallFrameSelection selection{
            .binding_coverage =
                std::vector<IndexCoverage>(plan.runtime.bindings.size()),
            .binding_regions =
                std::vector<std::vector<GlobalIndexRange>>(
                    plan.runtime.bindings.size()),
            .binding_upstream_coverage =
                std::vector<IndexCoverage>(plan.runtime.bindings.size()),
            .combine_sequential_input = graph_->root_operations.combine_sequential_input,
            .sample_rate = graph_->specialization.sample_rate,
        };
        for (graph_jit::BackgroundPortIndex port = 0;
             port < std::min(plan.ports.size(), plan.runtime.port_bindings.size()); ++port) {
            auto const& planned = plan.ports[port];
            auto const slot = plan.runtime.port_bindings[port];
            if (slot && planned.direction == graph_jit::PortDirection::input
                && planned.input_access == InputAccessKind::sequential) {
                selection.binding_upstream_coverage[*slot] = prepared_->input_coverage(port);
            }
        }
        auto assign_intersecting_regions = [](
            std::vector<GlobalIndexRange>& target,
            std::span<GlobalIndexRange const> regions,
            IndexCoverage const& required) {
            target.clear();
            for (auto const region : regions) {
                if (!(IndexCoverage{region} & required).empty()) {
                    target.push_back(region);
                }
            }
        };
        for (auto const& required : prepared_->result().input_requirements) {
            // A disconnected Random Access input has empty coverage, so its
            // requirement selects nothing.
            if (graph_jit::detail::is_disconnected_random_access_input(
                    plan, required.port)) {
                continue;
            }
            auto const slot = plan.runtime.port_bindings[required.port];
            if (slot) {
                selection.binding_coverage[*slot].include(required.required);
                auto const& port = plan.ports[required.port];
                if (port.input_access == InputAccessKind::sequential) {
                    auto const& coverage = selection.binding_coverage[*slot];
                    selection.binding_regions[*slot].assign(
                        coverage.begin(), coverage.end());
                } else {
                    auto const regions = prepared_->requested_input_regions(required.port);
                    selection.binding_regions[*slot].assign(regions.begin(), regions.end());
                }
            }
        }
        for (auto const& required : prepared_->result().output_requirements) {
            auto const slot = plan.runtime.port_bindings[required.port];
            if (slot && node_evaluates(plan.ports[required.port].node)) {
                selection.binding_coverage[*slot].include(required.required);
                auto const& port = plan.ports[required.port];
                if (port.output_production == OutputProductionKind::sequential) {
                    auto const& coverage = selection.binding_coverage[*slot];
                    selection.binding_regions[*slot].assign(
                        coverage.begin(), coverage.end());
                } else {
                    auto const regions =
                        prepared_->output_regions(required.port);
                    assign_intersecting_regions(
                        selection.binding_regions[*slot],
                        regions,
                        selection.binding_coverage[*slot]);
                }
            }
        }
        return selection;
    }

    [[nodiscard]] BackgroundStorageSelection storage_selection(
        BackgroundEvaluationCallFrameSelection const& bindings,
        PersistedDataStore::Snapshot const& published) const
    {
        auto const& plan = graph_->background_evaluation_plan;
        BackgroundStorageSelection selection{
            .generation = graph_->project_generation,
            .storage_coverage =
                std::vector<IndexCoverage>(plan.storage.ports.size()),
            .storage_regions = std::vector<std::vector<GlobalIndexRange>>(
                plan.storage.ports.size()),
            .published_output_regions =
                std::vector<std::vector<GlobalIndexRange>>(
                    plan.storage.ports.size()),
            .persisted_replacement_coverage =
                std::vector<IndexCoverage>(plan.storage.ports.size()),
            .produce_storage =
                std::vector<bool>(plan.storage.ports.size(), false),
            .published = &published,
        };
        selection.cache_computed_outputs = bool(inputs_.reconstruction_graph);

        for (graph_jit::PortStorageIndex index = 0;
             index < plan.storage.ports.size(); ++index) {
            auto const& storage = plan.storage.ports[index];
            if (!storage.output_port) {
                continue;
            }
            // Canonical Tick persistence has positional writes, not authored
            // Regions. Optional computed replay caches use logical Regions to
            // retain their known coverage independently of prepared windows.
            if (plan.ports[*storage.output_port].output_production
                == OutputProductionKind::sequential
                && (storage.storage == graph_jit::PortStorageKind::persisted_data
                    || !uses_output_cache(plan, storage, bool(inputs_.reconstruction_graph)))) {
                continue;
            }
            auto const regions = prepared_->output_regions(
                *storage.output_port);
            selection.published_output_regions[index].assign(
                regions.begin(), regions.end());
        }

        for (graph_jit::BackgroundBindingSlot slot = 0;
             slot < plan.runtime.bindings.size(); ++slot) {
            auto const& binding = plan.runtime.bindings[slot];
            auto const& coverage = bindings.binding_coverage[slot];
            if (coverage.empty()) continue;
            for (auto const storage_index : binding.storage) {
                bool direct = false;
                if (binding.kind == PortKind::sample &&
                    binding.direction == graph_jit::PortDirection::input) {
                    auto const& port = plan.ports[binding.port];
                    for (auto const& route : plan.storage.direct_samples) {
                        if (route.storage != storage_index ||
                            route.target_subset >=
                                plan.sample_target_subsets.size() ||
                            plan.sample_target_subsets[route.target_subset]
                                    .port != port.configured_port) {
                            continue;
                        }
                        selection.storage_coverage[storage_index].include(
                            shifted_back(coverage, route.read_latency));
                        direct = true;
                    }
                }
                if (!direct) {
                    selection.storage_coverage[storage_index].include(coverage);
                }
            }
        }

        // Tick-only Random Access consumers have no authored-Tock/replay call
        // frame binding. Their compiler-retained Tick slots still select the
        // storage that this transaction must make available before playback.
        for (auto const& required : prepared_->result().input_requirements) {
            if (required.port >= plan.ports.size()) continue;
            auto const& port = plan.ports[required.port];
            if (!port.random_access_input) continue;
            auto select_storage = [&](graph_jit::PortStorageIndex storage) {
                if (storage >= plan.storage.ports.size()) return;
                bool direct = false;
                if (port.kind == PortKind::sample) {
                    for (auto const& route : plan.storage.direct_samples) {
                        if (route.storage != storage
                            || route.target_subset
                                >= plan.sample_target_subsets.size()
                            || plan.sample_target_subsets[route.target_subset]
                                    .port
                                != port.configured_port) {
                            continue;
                        }
                        selection.storage_coverage[storage].include(
                            shifted_back(required.required, route.read_latency));
                        direct = true;
                    }
                } else {
                    for (auto const& route : plan.storage.direct_events) {
                        if (route.storage != storage
                            || route.target_subset
                                >= plan.event_target_subsets.size()) {
                            continue;
                        }
                        auto const& target =
                            plan.event_target_subsets[route.target_subset].port;
                        if (target.bundle
                                != port.configured_port.node_bundle_handle
                            || target.port
                                != port.configured_port.port_index) {
                            continue;
                        }
                        selection.storage_coverage[storage].include(
                            required.required);
                        direct = true;
                    }
                }
                if (!direct) {
                    selection.storage_coverage[storage].include(
                        required.required);
                }
            };
            auto const& bindings = port.kind == PortKind::sample
                ? plan.tick_runtime.random_access_sample_inputs
                : plan.tick_runtime.random_access_event_inputs;
            for (auto const& binding : bindings) {
                if (binding.port != required.port) continue;
                for (auto const storage : binding.storage) {
                    select_storage(storage);
                }
            }
        }

        // A valid retained output can satisfy a terminal request directly from
        // the pinned snapshot even when its producer is not scheduled.
        for (auto const& required : prepared_->result().output_requirements) {
            if (node_evaluates(plan.ports[required.port].node)) continue;
            for (graph_jit::PortStorageIndex index = 0;
                 index < plan.storage.ports.size(); ++index) {
                auto const& storage = plan.storage.ports[index];
                if (uses_output_cache(plan, storage, bool(inputs_.reconstruction_graph)) &&
                    storage.output_port == required.port) {
                    selection.storage_coverage[index].include(
                        required.required);
                }
            }
        }

        auto select_complete_regions =
            [&](graph_jit::PortStorageIndex index) {
            auto const& storage = plan.storage.ports[index];

            std::vector<GlobalIndexRange> selected_regions;
            auto append_intersecting = [&](std::span<GlobalIndexRange const> regions) {
                for (auto const region : regions) {
                    if (!(IndexCoverage{region}
                            & selection.storage_coverage[index]).empty()) {
                        selected_regions.push_back(region);
                    }
                }
            };
            if (storage.storage == graph_jit::PortStorageKind::persisted_data
                && storage.output_port
                && plan.ports[*storage.output_port].persisted_tick_output) {
                // Recording queries select positions within the published
                // logical Regions. Keep those descriptors intact: the reader
                // matches Region identity, not the requested subwindow.
                auto const output = data_->resolve_output(persisted_output_id(
                    plan, index, graph_->project_generation));
                if (storage.kind == PortKind::sample) {
                    for (auto const& region : published.sample_regions(output, storage.sample_layout)) {
                        if (region.geometry == storage.continuous_geometry
                            && selection.storage_coverage[index].intersects(region.range))
                            selected_regions.push_back(region.range);
                    }
                } else {
                    append_intersecting(published.event_region_ranges(output, storage.event_type));
                }
            } else if (storage.sample_materialization) {
                auto const& materialization = plan.storage
                    .sample_materializations[*storage.sample_materialization];
                auto const sequential = materialization.target_port
                        < plan.ports.size()
                    && plan.ports[materialization.target_port].input_access
                        == InputAccessKind::sequential;
                if (!sequential) {
                    append_intersecting(prepared_->input_regions(
                        materialization.target_port));
                }
            } else if (storage.event_materialization) {
                auto const& materialization = plan.storage
                    .event_materializations[*storage.event_materialization];
                auto const sequential = materialization.target_port
                    && *materialization.target_port < plan.ports.size()
                    && plan.ports[*materialization.target_port].input_access
                        == InputAccessKind::sequential;
                if (materialization.target_port && !sequential) {
                    append_intersecting(prepared_->input_regions(
                        *materialization.target_port));
                }
            } else if (storage.output_port
                && (plan.ports[*storage.output_port].output_production
                    != OutputProductionKind::sequential
                    || (storage.storage != graph_jit::PortStorageKind::persisted_data
                        && uses_output_cache(plan, storage, bool(inputs_.reconstruction_graph))))) {
                append_intersecting(
                    prepared_->output_regions(*storage.output_port));
            } else {
                // A shared materialization can serve several equivalent target
                // subsets. Its Region decomposition is the decomposition of
                // one complete logical target port, not one copy per channel
                // subset or per consumer.
                for (auto const subset : storage.target_subsets) {
                    auto const found = std::ranges::find_if(
                        plan.ports,
                        [&](graph_jit::BackgroundPortPlan const& port) {
                            if (port.direction
                                    != graph_jit::PortDirection::input
                                || port.kind != storage.kind) {
                                return false;
                            }
                            if (storage.kind == PortKind::sample) {
                                return subset < plan.sample_target_subsets.size()
                                    && port.configured_port
                                        == plan.sample_target_subsets[subset].port;
                            }
                            return subset < plan.event_target_subsets.size()
                                && port.configured_port.node_bundle_handle
                                    == plan.event_target_subsets[subset]
                                        .port.bundle
                                && port.configured_port.port_index
                                    == plan.event_target_subsets[subset]
                                        .port.port;
                        });
                    if (found == plan.ports.end()) continue;
                    auto const port_index = static_cast<
                        graph_jit::BackgroundPortIndex>(std::distance(
                            plan.ports.begin(), found));
                    append_intersecting(
                        prepared_->input_regions(port_index));
                    if (!selected_regions.empty()) break;
                }
            }
            if (selected_regions.empty()) {
                selected_regions.assign(
                    selection.storage_coverage[index].begin(),
                    selection.storage_coverage[index].end());
            }

            auto const changed = selected_regions != selection.storage_regions[index];
            selection.storage_regions[index] = std::move(selected_regions);
            return changed;
        };

        bool changed = true;
        while (changed) {
            changed = false;
            for (graph_jit::PortStorageIndex index = 0;
                 index < plan.storage.ports.size(); ++index) {
                changed = select_complete_regions(index) || changed;
            }
            for (auto const& materialization :
                 plan.storage.sample_materializations) {
                auto const target =
                    selection.storage_coverage[materialization.output];
                for (std::size_t source = 0;
                     source < materialization.inputs.size(); ++source) {
                    auto& selected =
                        selection
                            .storage_coverage[materialization.inputs[source]];
                    auto expanded =
                        selected |
                        shifted_back(
                            target,
                            materialization.source_read_latencies[source]);
                    if (expanded != selected) {
                        selected = std::move(expanded);
                        changed = true;
                    }
                }
            }
            for (auto const& materialization :
                 plan.storage.event_materializations) {
                auto const target =
                    selection.storage_coverage[materialization.output];
                for (auto const input : materialization.inputs) {
                    auto expanded = selection.storage_coverage[input] | target;
                    if (expanded != selection.storage_coverage[input]) {
                        selection.storage_coverage[input] = std::move(expanded);
                        changed = true;
                    }
                }
            }
        }

        for (graph_jit::PortStorageIndex index = 0;
             index < plan.storage.ports.size(); ++index) {
            auto const& storage = plan.storage.ports[index];
            if (!runtime_produced(plan, storage) ||
                selection.storage_coverage[index].empty()) {
                continue;
            }
            if (storage.sample_materialization ||
                storage.event_materialization) {
                selection.produce_storage[index] = true;
            } else if (storage.output_port) {
                selection.produce_storage[index] =
                    node_evaluates(plan.ports[*storage.output_port].node);
            }
        }
        for (graph_jit::PortStorageIndex index = 0;
             index < plan.storage.ports.size(); ++index) {
            auto const& storage = plan.storage.ports[index];
            if (!uses_output_cache(plan, storage, bool(inputs_.reconstruction_graph))
                || !storage.output_port) {
                continue;
            }
            if (selection.produce_storage[index]) {
                selection.persisted_replacement_coverage[index].include(
                    selection.storage_coverage[index]);
            }
            auto const output = data_->resolve_output(persisted_output_id(
                plan, index, graph_->project_generation));
            auto const mutation = std::ranges::find(
                data_mutations_, output, &DataMutation::output);
            if (mutation != data_mutations_.end()) {
                selection.persisted_replacement_coverage[index].include(
                    mutation->coverage);
            }
        }
        return selection;
    }

    void expand_output_binding_selection(
        BackgroundEvaluationCallFrameSelection& bindings,
        BackgroundStorageSelection const& storage) const
    {
        auto const& plan = graph_->background_evaluation_plan;
        for (graph_jit::BackgroundBindingSlot slot = 0;
             slot < plan.runtime.bindings.size(); ++slot) {
            auto const& binding = plan.runtime.bindings[slot];
            if (binding.direction != graph_jit::PortDirection::output
                || binding.port >= plan.ports.size()
                || !node_evaluates(plan.ports[binding.port].node)) {
                continue;
            }
            auto selected = bindings.binding_coverage[slot];
            for (auto const storage_index : binding.storage) {
                if (storage_index < storage.storage_coverage.size()) {
                    selected.include(
                        storage.storage_coverage[storage_index]);
                }
            }
            bindings.binding_coverage[slot] = selected;
            auto& regions = bindings.binding_regions[slot];
            regions.clear();
            if (plan.ports[binding.port].output_production
                == OutputProductionKind::sequential) {
                regions.assign(selected.begin(), selected.end());
                continue;
            }
            auto const published = prepared_->output_regions(binding.port);
            for (auto const region : published) {
                if (!(IndexCoverage{region} & selected).empty()) {
                    regions.push_back(region);
                }
            }
        }
    }

    struct Evaluation {
        std::unique_ptr<BackgroundStorageRealization> realization{};
        std::unique_ptr<BackgroundEvaluationCallFrame> frame{};
    };

    void retain_reconstruction_inputs(PersistedDataStore::Candidate& candidate)
    {
        if (!inputs_.reconstruction_graph) return;
        auto const& plan = graph_->background_evaluation_plan;
        for (std::size_t output = 0; output < plan.ports.size(); ++output) {
            auto const& port = plan.ports[output];
            if (port.direction != graph_jit::PortDirection::output || port.persisted_tick_output) continue;
            auto storage = std::ranges::find_if(plan.storage.ports, [&](auto const& slot) {
                return uses_output_cache(plan, slot, true) && slot.output_port == output;
            });
            if (storage == plan.storage.ports.end()) continue;
            auto const storage_index = static_cast<std::size_t>(storage - plan.storage.ports.begin());
            auto handle = data_->resolve_output(persisted_output_id(plan, storage_index, graph_->project_generation));
            bool const changed = std::ranges::any_of(prepared_->result().output_changes,
                [&](auto const& change) { return change.port == output && !change.changed.empty(); });
            if (!changed && candidate.working_snapshot().shares_output_with(pin_->snapshot(), handle)) continue;
            std::vector<bool> visited(plan.ports.size(), false);
            std::vector<PersistedOutputHandle> dependencies;
            auto visit = [&](auto&& self, std::size_t index) -> void {
                if (visited[index]) return;
                visited[index] = true;
                auto const& current = plan.ports[index];
                if (current.persisted_tick_output) {
                    auto identity = current.stable_identity
                        ? PersistedOutputId{*current.stable_identity}
                        : PersistedOutputId{GenerationLocalPersistedOutputId{
                            graph_->project_generation, index, current.kind}};
                    auto handle = data_->resolve_output(identity);
                    if (!std::ranges::contains(dependencies, handle)) dependencies.push_back(handle);
                    return;
                }
                if (current.direction == graph_jit::PortDirection::output) {
                    for (auto input : plan.nodes[current.node].inputs) self(self, input);
                } else for (auto connection : current.incoming_connections) {
                    auto const& edge = plan.connections[connection];
                    for (auto source : edge.sample_source_port_by_channel) if (source) self(self, *source);
                    for (auto const& delivery : edge.event_deliveries) if (delivery.source_port) self(self, *delivery.source_port);
                }
            };
            visit(visit, output);
            std::ranges::sort(dependencies);
            auto inputs = std::make_shared<PersistedDataStore::Snapshot>(candidate.working_snapshot());
            // Only non-recomputable leaves are retained. Keeping intermediate
            // contexts here would build a chain of old cache roots per request.
            inputs->select_outputs(dependencies);
            auto context = std::make_shared<PersistedDataStore::Reconstruction>();
            context->executable = inputs_.reconstruction_graph;
            context->inputs = std::move(inputs);
            context->dependencies = std::move(dependencies);
            data_->set_cache_realization(handle, port.realization.value_or(OutputRealization::computed));
            candidate.set_reconstruction(handle, std::move(context), pin_->snapshot());
        }
    }

    [[nodiscard]] std::shared_ptr<Evaluation> prepare_evaluation()
    {
        auto binding = binding_selection();
        auto storage = storage_selection(binding, *working_data_);
        expand_output_binding_selection(binding, storage);
        std::vector<PersistedPayloadSelection> needed;
        std::vector<PersistedOutputHandle> accounted;
        std::size_t incoming_bytes = 0;
        auto const& plan = graph_->background_evaluation_plan;
        auto const page = data_->buffered_storage_usage().page_size;
        auto reserve = [&](std::size_t bytes) {
            if (bytes > std::numeric_limits<std::size_t>::max() - (page - 1))
                throw std::length_error("background cache demand exceeds addressable payload size");
            auto const rounded = (bytes + page - 1) / page * page;
            if (rounded > std::numeric_limits<std::size_t>::max() - incoming_bytes)
                throw std::length_error("background cache demand exceeds addressable payload size");
            incoming_bytes += rounded;
        };
        for (std::size_t index = 0; index < plan.storage.ports.size(); ++index) {
            auto const& slot = plan.storage.ports[index];
            if (!uses_output_cache(plan, slot, bool(inputs_.reconstruction_graph)) || !slot.output_port) continue;
            auto output = data_->resolve_output(persisted_output_id(plan, index, graph_->project_generation));
            needed.push_back({output, storage.storage_coverage[index]});
            auto const& port = plan.ports[*slot.output_port];
            data_->set_cache_realization(output, port.realization.value_or(OutputRealization::buffered));
            if (!storage.produce_storage[index] || std::ranges::contains(accounted, output)) continue;
            accounted.push_back(output);
            if (slot.kind == PortKind::sample) {
                auto const width = slot.continuous_geometry.value_size_bytes * channel_count(slot.sample_layout);
                for (auto region : storage.storage_regions[index]) {
                    for (auto part : storage.storage_coverage[index] & IndexCoverage{region}) {
                        if (width && part.extent > std::numeric_limits<std::size_t>::max() / width)
                            throw std::length_error("background cache demand exceeds addressable payload size");
                        auto bytes = static_cast<std::size_t>(part.extent) * width;
                        reserve(bytes);
                    }
                }
            } else {
                long double count = 0;
                if (slot.max_event_count) count = *slot.max_event_count;
                else for (auto range : storage.storage_coverage[index])
                    count += std::ceil(static_cast<long double>(range.extent) * slot.max_events_per_index);
                if (!std::isfinite(count) || count > std::numeric_limits<std::size_t>::max() / (2 * sizeof(TimedEvent)))
                    throw std::length_error("background event cache demand exceeds addressable payload size");
                reserve(static_cast<std::size_t>(count) * 2 * sizeof(TimedEvent));
            }
        }
        // Release private optional ownership before canonical cache roots
        // retire. Ordinary preparation keeps its shallow immutable base.
        data_->prepare_cache(incoming_bytes, needed, [&](auto keep) {
            pin_->retain_payload(keep, true);
            if (active_candidate_) active_candidate_->retain_payload(keep);
        });
        prepared_payload_ = needed;
        if (remaining_materialized_sample_bytes_) {
            auto& remaining = *remaining_materialized_sample_bytes_;
            auto const& plan = graph_->background_evaluation_plan;
            for (std::size_t i = 0; i < plan.storage.ports.size(); ++i) {
                auto const& slot = plan.storage.ports[i];
                if (slot.kind != PortKind::sample || (uses_output_cache(plan, slot, bool(inputs_.reconstruction_graph))
                    && !storage.produce_storage[i])) continue;
                auto const bytes_per_frame = slot.continuous_geometry.value_size_bytes * channel_count(slot.sample_layout);
                if (!bytes_per_frame) continue;
                // Logical Regions can overlap; each owns its materialization.
                for (auto const region : storage.storage_regions[i]) {
                    auto const selected = storage.storage_coverage[i] & IndexCoverage{region};
                    for (auto const range : selected) {
                        if (range.extent > remaining / bytes_per_frame)
                            throw std::runtime_error("observation window exceeds its background materialization budget");
                        remaining -= static_cast<std::size_t>(range.extent) * bytes_per_frame;
                    }
                }
            }
        }
        auto realization = std::make_unique<BackgroundStorageRealization>(
            graph_->background_evaluation_plan,
            *data_,
            std::move(storage),
            graph_->root_operations.combine_sequential_input,
            graph_->root_operations.initialize_sequential_input);
        if (auto sealed = realization->seal(); !sealed) {
            throw std::runtime_error(sealed.error());
        }

        auto frame = std::make_unique<BackgroundEvaluationCallFrame>(
            graph_->background_evaluation_plan, *realization, *tock_states_, binding);
        for (std::size_t node = 0; node < prepared_->node_activity().size();
             ++node) {
            frame->nodes()[node].activity = prepared_->node_activity()[node];
        }
        auto const& runtime = graph_->background_evaluation_plan.runtime;
        for (graph_jit::BackgroundReplayInvocationSlot slot = 0;
             slot < runtime.replay_invocations.size(); ++slot) {
            IndexCoverage requested;
            auto const& replay = runtime.replay_invocations[slot];
            for (auto const binding_slot : replay.output_bindings) {
                requested.include(binding.binding_coverage[binding_slot]);
            }
            if (requested.empty()) {
                for (auto const binding_slot : replay.input_bindings) {
                    requested.include(binding.binding_coverage[binding_slot]);
                }
            }
            auto set = frame->set_replay_regions(
                slot, replay_regions(requested, replay.maximum_block_size));
            if (!set) throw std::runtime_error(set.error());
        }
        if (auto sealed = frame->seal(); !sealed) {
            throw std::runtime_error(sealed.error());
        }

        return std::make_shared<Evaluation>(Evaluation{
            .realization = std::move(realization), .frame = std::move(frame)});
    }

    static BackgroundDiscoveredInput materialize_discovery(void* opaque,
        PreparedCoveragePropagation&& request, graph_jit::BackgroundPortIndex input)
    {
        auto& self = *static_cast<Impl*>(opaque);
        // Recursive discovery uses separate reverse selections. Save only the
        // active selection pointer/value; the selected data root stays fixed.
        auto previous = std::move(self.prepared_);
        self.prepared_.emplace(std::move(request));
        try {
            auto evaluation = self.prepare_evaluation();
            self.graph_->background_operations.evaluate(&evaluation->frame->call());
            if (auto complete = evaluation->frame->validate_evaluation(); !complete) {
                throw std::runtime_error(complete.error());
            }
            BackgroundDiscoveredInput result{.owner = evaluation};
            if (self.graph_->background_evaluation_plan.ports[input].kind == PortKind::sample) {
                result.continuous = evaluation->frame->discovery_continuous_input(input);
            } else {
                result.events = evaluation->frame->discovery_event_input(input);
            }
            self.prepared_ = std::move(previous);
            return result;
        } catch (...) {
            self.prepared_ = std::move(previous);
            throw;
        }
    }

    [[nodiscard]] std::expected<BackgroundEvaluationResult, std::string> run()
    {
        if (executed_) {
            return std::unexpected(
                "background evaluation transaction already executed");
        }
        executed_ = true;
        if (inputs_.reconstruction && !inputs_.prepare_only)
            return std::unexpected("historical reconstruction cannot publish canonical data");
        auto const plan_is_empty = graph_->background_evaluation_plan.nodes.empty()
            && graph_->background_evaluation_plan.ports.empty()
            && graph_->background_evaluation_plan.storage.ports.empty();
        auto const roots_are_empty = inputs_.roots.locally_changed_nodes.empty()
            && inputs_.roots.forward_nodes.empty()
            && inputs_.roots.input_changes.empty()
            && inputs_.roots.output_changes.empty()
            && inputs_.roots.input_demands.empty()
            && inputs_.roots.output_demands.empty();
        auto const retirement_only = !graph_->background_operations.valid()
            && plan_is_empty && roots_are_empty
            && produced_routes_.empty() && produced_prefixes_.empty()
            && !inputs_.retired_outputs.empty();
        if (!graph_->background_operations.valid() && !retirement_only) {
            return std::unexpected(
                "active graph has no complete background operations");
        }
        reader_ = data_->register_reader();
        pin_.emplace(reader_.pin());
        pin_->detach();
        auto const& base = pin_->snapshot();
        semantic_version_ = inputs_.semantic_version.value_or(
            base.version().semantic);
        if (semantic_version_ < base.version().semantic) {
            return std::unexpected(
                "background evaluation semantic version cannot move backwards");
        }
        auto decoded = decode_produced_records();
        if (!decoded) return std::unexpected(std::move(decoded.error()));
        if (!decoded->empty()) {
            auto const page = data_->buffered_storage_usage().page_size;
            std::size_t incoming = 0;
            for (auto const& record : *decoded) {
                auto const bytes = record.header.payload_size;
                if (incoming > std::numeric_limits<std::size_t>::max() - 4 * page
                    || bytes > (std::numeric_limits<std::size_t>::max() - incoming - 4 * page) / 2)
                    throw std::length_error("recording batch exceeds addressable backing size");
                // Page-edge copy-on-write and an event timeline copy are
                // included conservatively. The allocator enforces actual use.
                incoming += 2 * (bytes + 2 * page);
            }
            data_->prepare_cache(incoming, {}, [&](auto keep) { pin_->retain_payload(keep, true); });
        }
        std::optional<PersistedDataStore::Candidate> candidate;
        if (!decoded->empty() || !inputs_.retired_outputs.empty() || inputs_.reusable_data || inputs_.reconstruction) {
            candidate.emplace(data_->begin_candidate(semantic_version_));
            if (candidate->base_version() != base.version()) {
                return BackgroundEvaluationResult{
                    .status = BackgroundEvaluationStatus::stale_base,
                };
            }
            if (inputs_.reusable_data) {
                auto const& plan = graph_->background_evaluation_plan;
                for (graph_jit::PortStorageIndex storage = 0; storage < plan.storage.ports.size(); ++storage) {
                    auto const& selected = plan.storage.ports[storage];
                    if (!uses_output_cache(plan, selected, bool(inputs_.reconstruction_graph)) || !selected.output_port
                        || plan.ports[*selected.output_port].persisted_tick_output) continue;
                    candidate->reuse_output(*inputs_.reusable_data,
                        data_->resolve_output(persisted_output_id(plan, storage, graph_->project_generation)));
                }
            }
            if (inputs_.reconstruction) {
                for (auto dependency : inputs_.reconstruction->dependencies)
                    candidate->reuse_output(*inputs_.reconstruction->inputs, dependency);
                // Cached results in the current root may have different
                // recorded inputs. Reconstruct privately from the selected
                // boundaries, with no incidental result-cache reuse.
                candidate->retain_payload({});
            }
            for (auto const output : inputs_.retired_outputs) candidate->erase_output(output);
            if (auto applied = apply_produced_records(
                    *decoded, candidate->working_snapshot(), *candidate);
                !applied) {
                return std::unexpected(std::move(applied.error()));
            }
        }
        if (retirement_only) {
            auto const published = candidate->target_version();
            if (data_->publish(std::move(*candidate)) == PersistedDataPublishResult::stale_base)
                return BackgroundEvaluationResult{.status = BackgroundEvaluationStatus::stale_base};
            std::optional<std::uint64_t> promoted;
            if (materializations_) promoted = materializations_->promote(
                std::make_unique<TickMaterializationSnapshot>(graph_->project_generation,
                    semantic_version_, published, std::vector<TickMaterializedSampleInput>{},
                    std::vector<TickMaterializedEventInput>{}));
            return BackgroundEvaluationResult{.published_data = published,
                .promoted_tick_materialization = promoted};
        }
        active_candidate_ = candidate ? &*candidate : nullptr;
        working_data_ = candidate ? &candidate->working_snapshot() : &base;
        if (inputs_.reusable_materialization)
            previous_materialization_ = inputs_.reusable_materialization;
        if (materializations_ && !previous_materialization_) {
            auto const* published = materializations_->published_for_writer();
            if (published && published->generation() == graph_->project_generation)
                previous_materialization_ = published;
        }
        // Recording boundaries use the current canonical selection, even when
        // unchanged Tock results are shared from a private successor preview.
        // Once canonical evaluation is running for this generation, decoded
        // queue records already provide its exact changed ranges. Re-scanning
        // the whole recording here would invalidate unchanged materialization.
        auto const& plan = graph_->background_evaluation_plan;
        for (graph_jit::PortStorageIndex storage = 0; storage < plan.storage.ports.size(); ++storage) {
            auto const& selected = plan.storage.ports[storage];
            if (selected.storage != graph_jit::PortStorageKind::persisted_data || !selected.output_port) continue;
            auto const port_index = *selected.output_port;
            auto const& port = plan.ports[port_index];
            if (!port.persisted_tick_output) continue;
            if (!inputs_.reusable_data && previous_materialization_) continue;
            auto const handle = data_->resolve_output(persisted_output_id(plan, storage, graph_->project_generation));
            if (inputs_.reusable_data && inputs_.reusable_data->shares_output_with(base, handle)) continue;
            auto lookup = [&](PersistedDataStore::Snapshot const& snapshot) {
                return port.kind == PortKind::sample
                    ? snapshot.find_sample_coverage(handle, port.sample_layout, port.continuous_geometry)
                    : snapshot.find_event_coverage(handle, port.event_type);
            };
            auto const* current = lookup(inputs_.reconstruction ? *working_data_ : base);
            IndexCoverage changed = current ? *current : IndexCoverage{};
            if (inputs_.reusable_data) {
                if (auto old = lookup(*inputs_.reusable_data)) changed.include(*old);
            }
            auto existing = std::ranges::find(inputs_.roots.output_changes, port_index, &OutputCoverageChangeRequest::port);
            if (existing == inputs_.roots.output_changes.end())
                inputs_.roots.output_changes.push_back({.port = port_index,
                    .coverage = current ? *current : IndexCoverage{}, .changed = std::move(changed)});
            else {
                // Decoded queue records already selected a newer working root.
                existing->changed.include(changed);
            }
        }
        prepared_.emplace(
            propagation_->prepare(graph_->background_operations,
                                  *coverage_, inputs_.roots,
                                  BackgroundCoverageDemandExpansion{
                                      .data = this,
                                      .expand_output = &expand_output_demand,
                                      .restrict_input = &restrict_input_demand,
                                      .cached_output = &cached_output,
                                  },
                                  BackgroundCoverageDiscovery{
                                      .data = this,
                                      .materialize = &materialize_discovery,
                                  }));

        auto evaluation = prepare_evaluation();
        auto& realization = evaluation->realization;
        auto& frame = *evaluation->frame;

        auto const& storage_selection = realization->selection();
        bool has_produced_data = false;
        for (graph_jit::PortStorageIndex index = 0;
             index < graph_->background_evaluation_plan.storage.ports.size();
             ++index) {
            auto const& planned =
                graph_->background_evaluation_plan.storage.ports[index];
            if (uses_output_cache(plan, planned, bool(inputs_.reconstruction_graph)) &&
                storage_selection.produce_storage[index] &&
                !storage_selection.storage_coverage[index].empty()) {
                has_produced_data = true;
                break;
            }
        }
        if (has_produced_data || !data_mutations_.empty()) {
            if (!candidate) {
                candidate.emplace(data_->begin_candidate(semantic_version_));
            }
            active_candidate_ = &*candidate;
            if (candidate->base_version() != pin_->snapshot().version()) {
                discard_prepared();
                return BackgroundEvaluationResult{
                    .status = BackgroundEvaluationStatus::stale_base,
                };
            }
            for (auto const& mutation : data_mutations_) {
                for (auto const range : mutation.coverage) {
                    if (mutation.kind == PortKind::event) {
                        candidate->erase_events(
                            mutation.output, range, mutation.event_type);
                    }
                }
            }
            working_data_ = &candidate->working_snapshot();
        }

        graph_->background_operations.evaluate(&frame.call());
        if (auto complete = frame.validate_evaluation(); !complete) {
            return std::unexpected(std::move(complete.error()));
        }

        if (inputs_.projection.project)
            inputs_.projection.project(inputs_.projection.data, plan, *prepared_, *realization,
                candidate ? candidate->target_version() : base.version());

        std::unique_ptr<TickMaterializationSnapshot> tick_materialization;
        if (materializations_) {
            std::vector<IndexCoverage> tick_input_requirements(plan.ports.size());
            for (auto const& input : prepared_->result().input_requirements)
                tick_input_requirements[input.port].include(input.required);
            auto frozen = realization->make_tick_materialization_snapshot(
                graph_->project_generation,
                semantic_version_,
                candidate ? candidate->target_version() : base.version(),
                tick_input_requirements);
            if (!frozen) {
                return std::unexpected(std::move(frozen.error()));
            }
            tick_materialization = std::move(*frozen);
            tick_materialization->set_output_coverages(prepared_->output_coverages());
            if (previous_materialization_
                && !materialization_retention_.empty()) {
                tick_materialization->carry_forward(
                    *previous_materialization_, materialization_retention_);
            }
            // All allocation/faulting/locking precedes publication. The audio
            // callback adopts the immutable payload and its residency lease.
            tick_materialization->prepare_resident(*data_);
        }

        if (inputs_.prepare_only) {
            if (candidate) {
                if (auto staged = realization->stage_persisted_data(*candidate); !staged)
                    return std::unexpected(std::move(staged.error()));
                retain_reconstruction_inputs(*candidate);
            }
            if (!data_->is_current(base)) {
                discard_prepared();
                return BackgroundEvaluationResult{.status = BackgroundEvaluationStatus::stale_base};
            }
            auto selected = std::make_unique<PersistedDataStore::Snapshot>(
                candidate ? candidate->working_snapshot() : base);
            selected->retain_payload(prepared_payload_);
            auto result = propagation_->commit(*coverage_, std::move(*prepared_));
            prepared_.reset();
            return BackgroundEvaluationResult{
                .coverage = std::move(result),
                .prepared_data = std::move(selected),
                .prepared_materialization = std::move(tick_materialization),
            };
        }
        std::optional<PersistedDataVersion> published;
        if (candidate) {
            if (auto staged = realization->stage_persisted_data(*candidate);
                !staged) {
                return std::unexpected(std::move(staged.error()));
            }
            retain_reconstruction_inputs(*candidate);
            published = candidate->target_version();
            if (data_->publish(std::move(*candidate)) ==
                PersistedDataPublishResult::stale_base) {
                discard_prepared();
                return BackgroundEvaluationResult{
                    .status = BackgroundEvaluationStatus::stale_base,
                };
            }
        } else if (!data_->is_current(pin_->snapshot())) {
            discard_prepared();
            return BackgroundEvaluationResult{
                .status = BackgroundEvaluationStatus::stale_base,
            };
        }

        std::optional<std::uint64_t> promoted_tick_materialization;
        if (tick_materialization) {
            promoted_tick_materialization = materializations_->promote(
                std::move(tick_materialization));
        }

        auto result = propagation_->commit(*coverage_, std::move(*prepared_));
        prepared_.reset();
        return BackgroundEvaluationResult{
            .status = BackgroundEvaluationStatus::committed,
            .coverage = std::move(result),
            .published_data = published,
            .promoted_tick_materialization =
                promoted_tick_materialization,
        };
    }

public:
    Impl(CompiledGraph const& graph, TockStateStorage& tock_states,
         BackgroundCoverageState& coverage,
         BackgroundPropagationWorkspace& propagation, PersistedDataStore& data,
         TickMaterializationStore* materializations,
         BackgroundTransactionInputs inputs,
         std::span<BackgroundProducedInputRoute const> produced_routes = {},
         std::span<PinnedBlockPrefix const> produced_prefixes = {})
        : graph_(&graph)
        , tock_states_(&tock_states)
        , coverage_(&coverage)
        , propagation_(&propagation)
        , data_(&data)
        , materializations_(materializations)
        , inputs_(std::move(inputs))
        , remaining_materialized_sample_bytes_(inputs_.maximum_materialized_sample_bytes)
        , produced_routes_(produced_routes)
        , produced_prefixes_(produced_prefixes)
    {}

    ~Impl()
    {
        discard_prepared();
        release_reader();
    }

    [[nodiscard]] std::expected<BackgroundEvaluationResult, std::string>
    execute()
    {
        try {
            auto result = run();
            if (!result) discard_prepared();
            release_reader();
            return result;
        } catch (std::exception const& error) {
            discard_prepared();
            release_reader();
            return std::unexpected(error.what());
        } catch (...) {
            discard_prepared();
            release_reader();
            return std::unexpected(
                "background evaluation failed with an unknown exception");
        }
    }
};

std::expected<BackgroundEvaluationResult, std::string>
BackgroundEvaluationTransaction::reconstruct(PersistedDataStore& data,
    std::shared_ptr<PersistedDataStore::Reconstruction const> context,
    graph_jit::BackgroundPortIndex output, IndexCoverage required,
    ResourceContext const& resources)
{
    try {
        if (!context || !context->executable || !context->inputs)
            return std::unexpected("reconstruction has no immutable producer/input selection");
        auto const& graph = *context->executable;
        auto const& plan = graph.background_evaluation_plan;
        if (output >= plan.ports.size() || plan.ports[output].direction != graph_jit::PortDirection::output
            || plan.ports[output].persisted_tick_output)
            return std::unexpected("reconstruction requires a recomputable output");
        auto states = graph.tock_state_layout.create_storage(resources);
        states.initialize();
        BackgroundCoverageState coverage(plan.accumulators.output_change_count);
        BackgroundPropagationWorkspace propagation(plan, graph.specialization.sample_rate);
        CoveragePropagationRequest roots;
        std::vector<bool> visited(plan.nodes.size(), false);
        auto visit = [&](auto&& self, std::size_t node) -> void {
            if (visited[node]) return;
            visited[node] = true;
            roots.locally_changed_nodes.push_back(node);
            for (auto input : plan.nodes[node].inputs)
                for (auto connection : plan.ports[input].incoming_connections) {
                    auto const& edge = plan.connections[connection];
                    for (auto source : edge.sample_source_port_by_channel)
                        if (source) self(self, plan.ports[*source].node);
                    for (auto const& delivery : edge.event_deliveries)
                        if (delivery.source_port) self(self, plan.ports[*delivery.source_port].node);
                }
        };
        visit(visit, plan.ports[output].node);
        roots.output_demands.push_back({output, std::move(required)});
        BackgroundEvaluationTransaction transaction{graph, states, coverage, propagation, data,
            {.roots = std::move(roots), .prepare_only = true,
             .reconstruction_graph = context->executable, .reconstruction = std::move(context)}};
        return transaction.execute();
    } catch (std::exception const& error) {
        return std::unexpected(error.what());
    }
}

BackgroundEvaluationTransaction::BackgroundEvaluationTransaction(
    CompiledGraph const& graph, TockStateStorage& tock_states,
    BackgroundCoverageState& coverage,
    BackgroundPropagationWorkspace& propagation, PersistedDataStore& data,
    BackgroundTransactionInputs inputs)
    : impl_(std::make_unique<Impl>(graph, tock_states, coverage, propagation,
                                   data, nullptr, std::move(inputs)))
{}

BackgroundEvaluationTransaction::BackgroundEvaluationTransaction(
    CompiledGraph const& graph, TockStateStorage& tock_states,
    BackgroundCoverageState& coverage,
    BackgroundPropagationWorkspace& propagation, PersistedDataStore& data,
    TickMaterializationStore& materializations,
    BackgroundTransactionInputs inputs)
    : impl_(std::make_unique<Impl>(
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        &materializations,
        std::move(inputs)))
{}

BackgroundEvaluationTransaction::BackgroundEvaluationTransaction(
    CompiledGraph const& graph, TockStateStorage& tock_states,
    BackgroundCoverageState& coverage,
    BackgroundPropagationWorkspace& propagation, PersistedDataStore& data,
    TickMaterializationStore& materializations,
    BackgroundTransactionInputs inputs,
    std::span<BackgroundProducedInputRoute const> produced_routes,
    std::span<PinnedBlockPrefix const> produced_prefixes)
    : impl_(std::make_unique<Impl>(
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        &materializations,
        std::move(inputs),
        produced_routes,
        produced_prefixes))
{}

BackgroundEvaluationTransaction::~BackgroundEvaluationTransaction() = default;
BackgroundEvaluationTransaction::BackgroundEvaluationTransaction(
    BackgroundEvaluationTransaction&&) noexcept = default;
BackgroundEvaluationTransaction& BackgroundEvaluationTransaction::operator=(
    BackgroundEvaluationTransaction&&) noexcept = default;

std::expected<BackgroundEvaluationResult, std::string>
BackgroundEvaluationTransaction::execute()
{
    if (!impl_) {
        return std::unexpected(
            "background evaluation transaction has been moved from");
    }
    return impl_->execute();
}

} // namespace iv
