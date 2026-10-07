#include <intravenous/runtime/background_evaluation_transaction.h>

#include <intravenous/runtime/background_evaluation_call_frame.h>
#include <intravenous/runtime/background_storage_realization.h>
#include <intravenous/runtime/realtime_produced_record.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace iv {
namespace {

[[nodiscard]] bool contains_coverage(Coverage const& outer,
                                     Coverage const& inner) noexcept
{
    return std::ranges::all_of(inner.regions(), [&](IndexRegion region) {
        return outer.contains(region);
    });
}

[[nodiscard]] std::size_t coverage_sample_count(Coverage const& coverage)
{
    std::size_t result = 0;
    for (auto const region : coverage.regions()) {
        auto const count = static_cast<std::size_t>(region.end - region.begin);
        if (count > std::numeric_limits<std::size_t>::max() - result) {
            throw std::length_error("persisted coverage is too large");
        }
        result += count;
    }
    return result;
}

[[nodiscard]] std::optional<std::size_t> packed_frame_offset(
    Coverage const& domain, SampleIndex sample) noexcept
{
    std::size_t offset = 0;
    for (auto const region : domain.regions()) {
        if (region.contains(sample)) {
            return offset + static_cast<std::size_t>(sample - region.begin);
        }
        if (sample < region.begin) break;
        offset += static_cast<std::size_t>(region.end - region.begin);
    }
    return std::nullopt;
}

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
    case EventTypeId::empty:
        return std::holds_alternative<EmptyEvent>(event);
    case EventTypeId::count:
        return false;
    }
    return false;
}

[[nodiscard]] Coverage shifted_back(Coverage const& coverage,
                                    std::size_t latency)
{
    Coverage result;
    for (auto const region : coverage.regions()) {
        if (region.end <= latency) continue;
        result.include({
            region.begin < latency ? 0 : region.begin - latency,
            region.end - latency,
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

[[nodiscard]] std::vector<IndexRegion> replay_regions(Coverage const& coverage,
                                                      std::size_t maximum)
{
    if (maximum == 0 && !coverage.empty()) {
        throw std::invalid_argument(
            "background replay has no compiled block limit");
    }
    std::vector<IndexRegion> result;
    for (auto const region : coverage.regions()) {
        auto begin = region.begin;
        while (begin < region.end) {
            auto const end = std::min(
                region.end, saturating_sample_index_add(
                                begin, static_cast<SampleIndex>(maximum)));
            if (end <= begin) {
                throw std::length_error(
                    "background replay region cannot be split");
            }
            result.push_back({begin, end});
            begin = end;
        }
    }
    return result;
}

} // namespace

class BackgroundEvaluationTransaction::Impl {
    struct PageMutation {
        PersistedOutputHandle output{};
        std::uint64_t page = 0;
    };

    struct ProducedRecord {
        BackgroundProducedInputRoute route{};
        RealtimeProducedRecordHeader header{};
        std::vector<std::byte> payload{};
    };

    CompiledGraph const* graph_ = nullptr;
    std::byte* node_storage_ = nullptr;
    BackgroundCoverageState* coverage_ = nullptr;
    BackgroundPropagationWorkspace* propagation_ = nullptr;
    PersistedPageStore* pages_ = nullptr;
    TickMaterializationStore* materializations_ = nullptr;
    BackgroundTransactionInputs inputs_{};
    std::uint64_t semantic_version_ = 0;
    std::size_t page_width_ = 0;
    std::span<BackgroundProducedInputRoute const> produced_routes_{};
    std::span<PinnedBlockPrefix const> produced_prefixes_{};
    PersistedPageStore::ReaderSlot reader_{};
    std::optional<PersistedPageStore::ReaderPin> pin_{};
    std::optional<PreparedCoveragePropagation> prepared_{};
    std::vector<PageMutation> page_mutations_{};
    PersistedPageStore::Snapshot const* working_pages_ = nullptr;
    bool executed_ = false;

    void discard_prepared() noexcept
    {
        if (!prepared_) return;
        propagation_->discard(std::move(*prepared_));
        prepared_.reset();
    }

    void release_reader() noexcept
    {
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

    void add_page_mutation(PersistedOutputHandle output, std::uint64_t page)
    {
        if (!std::ranges::any_of(
                page_mutations_, [&](PageMutation const& item) {
                    return item.page == page && item.output == output;
                })) {
            page_mutations_.push_back({output, page});
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
                || port.retention != OutputRetention::persisted
                || persisted_output_kind(route.output) != route.kind) {
                return std::unexpected(
                    "background produced input has an incompatible persisted route");
            }

            bool canonical_route = false;
            for (graph_jit::PortStorageIndex storage = 0;
                 storage < plan.storage.ports.size(); ++storage) {
                auto const& planned = plan.storage.ports[storage];
                if (planned.storage
                        != graph_jit::PortStorageKind::persisted_pages
                    || planned.output_port != route.port
                    || planned.kind != route.kind) {
                    continue;
                }
                if (pages_->resolve_output(persisted_output_id(
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
            std::size_t remaining_blocks = 0;
            bool invalid = false;
            std::string error;
            prefix.for_each([&](AsyncQueueBlock const& block) {
                if (invalid) return;
                auto bytes = block.used_storage();
                if (!current) {
                    if (bytes.size() < sizeof(RealtimeProducedRecordHeader)) {
                        invalid = true;
                        error = "realtime-produced record header is incomplete";
                        return;
                    }
                    ProducedRecord record{.route = route};
                    std::memcpy(
                        &record.header, bytes.data(), sizeof(record.header));
                    if (record.header.record_block_count == 0) {
                        invalid = true;
                        error = "realtime-produced record has no physical blocks";
                        return;
                    }
                    record.payload.reserve(record.header.payload_size);
                    bytes = bytes.subspan(sizeof(record.header));
                    remaining_blocks = record.header.record_block_count;
                    current.emplace(std::move(record));
                }

                if (remaining_blocks == 0
                    || bytes.size()
                        > current->header.payload_size
                            - current->payload.size()) {
                    invalid = true;
                    error = "realtime-produced record payload exceeds its header";
                    return;
                }
                current->payload.insert(
                    current->payload.end(), bytes.begin(), bytes.end());
                --remaining_blocks;
                if (remaining_blocks != 0) return;
                if (current->payload.size() != current->header.payload_size) {
                    invalid = true;
                    error = "realtime-produced record payload is incomplete";
                    return;
                }
                records.push_back(std::move(*current));
                current.reset();
            });
            if (invalid) return std::unexpected(std::move(error));
            if (current || remaining_blocks != 0) {
                return std::unexpected(
                    "realtime-produced record ends before its declared block count");
            }
        }
        return records;
    }

    [[nodiscard]] Sample read_existing_sample(
        PersistedSamplePage const& page,
        SampleIndex sample,
        std::size_t channel,
        std::size_t page_width) const
    {
        auto frames = page_width;
        std::size_t frame = static_cast<std::size_t>(sample % page_width);
        if (page.packing == PersistedSamplePacking::coverage_packed) {
            auto const packed = packed_frame_offset(page.domain, sample);
            if (!packed) {
                throw std::logic_error(
                    "persisted sample page is missing selected coverage");
            }
            frame = *packed;
            frames = coverage_sample_count(page.domain);
        }
        auto const channels = channel_count(page.layout);
        auto const offset = page.layout.sample_layout
                == SampleStreamLayout::planar
            ? channel * frames + frame
            : frame * channels + channel;
        if (offset >= page.values.size()) {
            throw std::logic_error("persisted sample page payload is incomplete");
        }
        return page.values[offset];
    }

    [[nodiscard]] Sample read_produced_sample(
        ProducedRecord const& record,
        SampleIndex sample,
        std::size_t channel) const
    {
        auto const frame = static_cast<std::size_t>(sample - record.header.begin);
        auto const channels = channel_count(record.header.sample_layout);
        auto const offset = record.header.sample_layout.sample_layout
                == SampleStreamLayout::planar
            ? channel * record.header.sample_count + frame
            : frame * channels + channel;
        Sample value{};
        std::memcpy(
            &value,
            record.payload.data() + offset * sizeof(Sample),
            sizeof(value));
        return value;
    }

    [[nodiscard]] std::expected<void, std::string> apply_sample_record(
        ProducedRecord const& record,
        PersistedPageStore::Candidate& candidate)
    {
        auto const& header = record.header;
        auto const& port =
            graph_->background_evaluation_plan.ports[record.route.port];
        auto const channels = channel_count(header.sample_layout);
        if (header.payload_kind != RealtimeProducedPayloadKind::samples
            || record.route.kind != PortKind::sample
            || header.sample_layout != port.sample_layout
            || channels == 0
            || header.sample_count
                > std::numeric_limits<std::size_t>::max() / channels
            || header.sample_count * channels
                > std::numeric_limits<std::size_t>::max() / sizeof(Sample)
            || header.payload_size
                != header.sample_count * channels * sizeof(Sample)
            || header.sample_count == 0
            || header.begin > std::numeric_limits<SampleIndex>::max()
                    - header.sample_count) {
            return std::unexpected(
                "realtime-produced sample record has invalid metadata");
        }

        auto const end = header.begin
            + static_cast<SampleIndex>(header.sample_count);
        auto const width = candidate.page_width();
        auto page = static_cast<std::uint64_t>(header.begin / width);
        auto const last = static_cast<std::uint64_t>((end - 1) / width);
        for (;; ++page) {
            auto const page_begin = static_cast<SampleIndex>(page * width);
            auto const page_end = saturating_sample_index_add(
                page_begin, static_cast<SampleIndex>(width));
            auto const overwrite = IndexRegion{
                std::max(header.begin, page_begin), std::min(end, page_end)};
            auto const* existing = candidate.working_snapshot()
                .find_sample_page(record.route.output, page);
            if (existing && existing->layout != header.sample_layout) {
                return std::unexpected(
                    "realtime-produced sample layout disagrees with persisted pages");
            }
            Coverage domain = existing ? existing->domain : Coverage{};
            domain.include(overwrite);
            auto const frames = coverage_sample_count(domain);
            PersistedSamplePage replacement{
                .output = record.route.output,
                .page_index = page,
                .domain = domain,
                .layout = header.sample_layout,
                .packing = PersistedSamplePacking::coverage_packed,
            };
            replacement.values.resize(frames * channels);
            std::size_t packed_frame = 0;
            for (auto const part : domain.regions()) {
                for (auto sample = part.begin; sample < part.end;
                     ++sample, ++packed_frame) {
                    for (std::size_t channel = 0; channel < channels;
                         ++channel) {
                        auto const value = overwrite.contains(sample)
                            ? read_produced_sample(record, sample, channel)
                            : read_existing_sample(
                                  *existing, sample, channel, width);
                        auto const offset = header.sample_layout.sample_layout
                                == SampleStreamLayout::planar
                            ? channel * frames + packed_frame
                            : packed_frame * channels + channel;
                        replacement.values[offset] = value;
                    }
                }
            }
            candidate.put(std::move(replacement));
            if (page == last) break;
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> apply_event_record(
        ProducedRecord const& record,
        PersistedPageStore::Candidate& candidate)
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
            || header.sample_count == 0
            || header.begin > std::numeric_limits<SampleIndex>::max()
                    - header.sample_count) {
            return std::unexpected(
                "realtime-produced event record has invalid metadata");
        }
        auto const end = header.begin
            + static_cast<SampleIndex>(header.sample_count);
        std::vector<TimedEvent> captured(header.event_count);
        if (!captured.empty()) {
            std::memcpy(
                captured.data(), record.payload.data(), header.payload_size);
        }
        EventTime previous = 0;
        bool first = true;
        for (auto const& event : captured) {
            auto const time = static_cast<SampleIndex>(event.time);
            if (time < header.begin || time >= end
                || !event_matches_type(header.event_type, event.value)
                || (!first && event.time < previous)) {
                return std::unexpected(
                    "realtime-produced event payload is invalid");
            }
            previous = event.time;
            first = false;
        }

        auto const width = candidate.page_width();
        auto page = static_cast<std::uint64_t>(header.begin / width);
        auto const last = static_cast<std::uint64_t>((end - 1) / width);
        for (;; ++page) {
            auto const page_begin = static_cast<SampleIndex>(page * width);
            auto const page_end = saturating_sample_index_add(
                page_begin, static_cast<SampleIndex>(width));
            auto const overwrite = IndexRegion{
                std::max(header.begin, page_begin), std::min(end, page_end)};
            auto const* existing = candidate.working_snapshot()
                .find_event_page(record.route.output, page);
            if (existing && existing->type != header.event_type) {
                return std::unexpected(
                    "realtime-produced event type disagrees with persisted pages");
            }
            Coverage domain = existing ? existing->domain : Coverage{};
            domain.include(overwrite);
            PersistedEventPage replacement{
                .output = record.route.output,
                .page_index = page,
                .domain = std::move(domain),
                .type = header.event_type,
            };
            if (existing) {
                for (auto event : existing->events) {
                    auto const absolute = page_begin
                        + static_cast<SampleIndex>(event.time);
                    if (!overwrite.contains(absolute)) {
                        replacement.events.push_back(std::move(event));
                    }
                }
            }
            for (auto event : captured) {
                auto const absolute = static_cast<SampleIndex>(event.time);
                if (!overwrite.contains(absolute)) continue;
                event.time = static_cast<EventTime>(absolute - page_begin);
                replacement.events.push_back(std::move(event));
            }
            std::ranges::stable_sort(
                replacement.events, {}, &TimedEvent::time);
            candidate.put(std::move(replacement));
            if (page == last) break;
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> apply_sample_void(
        ProducedRecord const& record,
        PersistedPageStore::Candidate& candidate)
    {
        auto const& header = record.header;
        auto const& port =
            graph_->background_evaluation_plan.ports[record.route.port];
        if (header.payload_kind != RealtimeProducedPayloadKind::void_value
            || record.route.kind != PortKind::sample
            || header.sample_layout != port.sample_layout
            || header.payload_size != 0 || !record.payload.empty()
            || header.sample_count == 0
            || header.begin > std::numeric_limits<SampleIndex>::max()
                    - header.sample_count) {
            return std::unexpected(
                "realtime-produced sample void has invalid metadata");
        }

        auto const end = header.begin
            + static_cast<SampleIndex>(header.sample_count);
        auto const width = candidate.page_width();
        auto page = static_cast<std::uint64_t>(header.begin / width);
        auto const last = static_cast<std::uint64_t>((end - 1) / width);
        auto const channels = channel_count(header.sample_layout);
        for (;; ++page) {
            auto const page_begin = static_cast<SampleIndex>(page * width);
            auto const page_end = saturating_sample_index_add(
                page_begin, static_cast<SampleIndex>(width));
            auto const erased = IndexRegion{
                std::max(header.begin, page_begin), std::min(end, page_end)};
            auto const* existing = candidate.working_snapshot()
                .find_sample_page(record.route.output, page);
            if (existing) {
                if (existing->layout != header.sample_layout) {
                    return std::unexpected(
                        "realtime-produced sample void layout disagrees with persisted pages");
                }
                auto domain = existing->domain;
                domain.exclude(erased);
                if (domain.empty()) {
                    candidate.erase_page(record.route.output, page);
                } else if (domain != existing->domain) {
                    auto const frames = coverage_sample_count(domain);
                    PersistedSamplePage replacement{
                        .output = record.route.output,
                        .page_index = page,
                        .domain = domain,
                        .layout = header.sample_layout,
                        .packing = PersistedSamplePacking::coverage_packed,
                    };
                    replacement.values.resize(frames * channels);
                    std::size_t packed_frame = 0;
                    for (auto const part : domain.regions()) {
                        for (auto sample = part.begin; sample < part.end;
                             ++sample, ++packed_frame) {
                            for (std::size_t channel = 0; channel < channels;
                                 ++channel) {
                                auto const value = read_existing_sample(
                                    *existing, sample, channel, width);
                                auto const offset =
                                    header.sample_layout.sample_layout
                                        == SampleStreamLayout::planar
                                    ? channel * frames + packed_frame
                                    : packed_frame * channels + channel;
                                replacement.values[offset] = value;
                            }
                        }
                    }
                    candidate.put(std::move(replacement));
                }
            }
            if (page == last) break;
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> apply_event_void(
        ProducedRecord const& record,
        PersistedPageStore::Candidate& candidate)
    {
        auto const& header = record.header;
        auto const& port =
            graph_->background_evaluation_plan.ports[record.route.port];
        if (header.payload_kind != RealtimeProducedPayloadKind::void_value
            || record.route.kind != PortKind::event
            || header.event_type != port.event_type
            || header.payload_size != 0 || !record.payload.empty()
            || header.event_count != 0 || header.sample_count == 0
            || header.begin > std::numeric_limits<SampleIndex>::max()
                    - header.sample_count) {
            return std::unexpected(
                "realtime-produced event void has invalid metadata");
        }

        auto const end = header.begin
            + static_cast<SampleIndex>(header.sample_count);
        auto const width = candidate.page_width();
        auto page = static_cast<std::uint64_t>(header.begin / width);
        auto const last = static_cast<std::uint64_t>((end - 1) / width);
        for (;; ++page) {
            auto const page_begin = static_cast<SampleIndex>(page * width);
            auto const page_end = saturating_sample_index_add(
                page_begin, static_cast<SampleIndex>(width));
            auto const erased = IndexRegion{
                std::max(header.begin, page_begin), std::min(end, page_end)};
            auto const* existing = candidate.working_snapshot()
                .find_event_page(record.route.output, page);
            if (existing) {
                if (existing->type != header.event_type) {
                    return std::unexpected(
                        "realtime-produced event void type disagrees with persisted pages");
                }
                auto domain = existing->domain;
                domain.exclude(erased);
                if (domain.empty()) {
                    candidate.erase_page(record.route.output, page);
                } else if (domain != existing->domain) {
                    PersistedEventPage replacement{
                        .output = record.route.output,
                        .page_index = page,
                        .domain = std::move(domain),
                        .type = header.event_type,
                    };
                    for (auto const& event : existing->events) {
                        auto const absolute = page_begin
                            + static_cast<SampleIndex>(event.time);
                        if (!erased.contains(absolute)) {
                            replacement.events.push_back(event);
                        }
                    }
                    candidate.put(std::move(replacement));
                }
            }
            if (page == last) break;
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> apply_produced_records(
        std::span<ProducedRecord const> records,
        PersistedPageStore::Snapshot const& base,
        PersistedPageStore::Candidate& candidate)
    {
        struct Root {
            BackgroundProducedInputRoute route{};
            Coverage coverage{};
            Coverage changed{};
        };
        std::vector<Root> roots;
        for (auto const& record : records) {
            auto const end = saturating_sample_index_add(
                record.header.begin, record.header.sample_count);
            if (end <= record.header.begin) {
                return std::unexpected(
                    "realtime-produced record has an invalid timeline window");
            }
            auto const changed = IndexRegion{record.header.begin, end};
            auto found = std::ranges::find_if(roots, [&](Root const& root) {
                return root.route.port == record.route.port
                    && root.route.output == record.route.output;
            });
            if (found == roots.end()) {
                Coverage coverage;
                if (record.route.kind == PortKind::sample) {
                    auto const& layout = graph_->background_evaluation_plan
                        .ports[record.route.port].sample_layout;
                    if (auto const* existing =
                            base.find_sample_coverage(record.route.output, layout)) {
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
                auto removed = found->coverage & Coverage{changed};
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

    static void
    expand_output_demand(void* opaque, graph_jit::BackgroundPortIndex port,
                         Coverage const& available, Coverage const& changed,
                         Coverage const& required, Coverage& additional)
    {
        auto& self = *static_cast<Impl*>(opaque);
        auto const& plan = self.graph_->background_evaluation_plan;
        auto const& snapshot = *self.working_pages_;
        auto const width = self.page_width_;
        Coverage affected = required | changed;
        if (affected.empty()) return;

        for (graph_jit::PortStorageIndex index = 0;
             index < plan.storage.ports.size(); ++index) {
            auto const& storage = plan.storage.ports[index];
            if (storage.storage !=
                    graph_jit::PortStorageKind::persisted_pages ||
                storage.output_port != port ||
                !runtime_produced(plan, storage)) {
                continue;
            }
            auto const output = self.pages_->resolve_output(persisted_output_id(
                plan, index, self.graph_->project_generation));
            for (auto const region : affected.regions()) {
                auto page = static_cast<std::uint64_t>(region.begin / width);
                auto const last =
                    static_cast<std::uint64_t>((region.end - 1) / width);
                for (;; ++page) {
                    auto const page_begin =
                        static_cast<SampleIndex>(page * width);
                    auto const page_end = saturating_sample_index_add(
                        page_begin, static_cast<SampleIndex>(width));
                    auto const page_region = IndexRegion{page_begin, page_end};
                    auto const desired = available & Coverage{page_region};
                    auto const is_changed = changed.intersects(page_region);
                    Coverage existing_domain;
                    bool exists = false;
                    if (storage.kind == PortKind::sample) {
                        if (auto const* existing =
                                snapshot.find_sample_page(output, page)) {
                            existing_domain = existing->domain;
                            exists = true;
                        }
                    } else if (auto const* existing =
                                   snapshot.find_event_page(output, page)) {
                        existing_domain = existing->domain;
                        exists = true;
                    }
                    if (is_changed ||
                        (required.intersects(page_region) &&
                         (!exists ||
                          !contains_coverage(existing_domain, desired)))) {
                        additional.include(desired);
                    }
                    if (is_changed && exists) {
                        self.add_page_mutation(output, page);
                    }
                    if (page == last) break;
                }
            }
        }
    }

    [[nodiscard]] BackgroundEvaluationCallFrameSelection
    binding_selection() const
    {
        auto const& plan = graph_->background_evaluation_plan;
        BackgroundEvaluationCallFrameSelection selection{
            .binding_coverage =
                std::vector<Coverage>(plan.runtime.bindings.size()),
            .sample_rate = graph_->specialization.sample_rate,
        };
        for (auto const& required : prepared_->result().input_requirements) {
            auto const slot = plan.runtime.port_bindings[required.port];
            if (slot)
                selection.binding_coverage[*slot].include(required.required);
        }
        for (auto const& required : prepared_->result().output_requirements) {
            auto const slot = plan.runtime.port_bindings[required.port];
            if (slot && node_evaluates(plan.ports[required.port].node)) {
                selection.binding_coverage[*slot].include(required.required);
            }
        }
        return selection;
    }

    [[nodiscard]] BackgroundStorageSelection storage_selection(
        BackgroundEvaluationCallFrameSelection const& bindings,
        PersistedPageStore::Snapshot const& published) const
    {
        auto const& plan = graph_->background_evaluation_plan;
        BackgroundStorageSelection selection{
            .generation = graph_->project_generation,
            .storage_coverage =
                std::vector<Coverage>(plan.storage.ports.size()),
            .produce_storage =
                std::vector<bool>(plan.storage.ports.size(), false),
            .published = &published,
        };

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
                if (storage.storage ==
                        graph_jit::PortStorageKind::persisted_pages &&
                    storage.output_port == required.port) {
                    selection.storage_coverage[index].include(
                        required.required);
                }
            }
        }

        bool changed = true;
        while (changed) {
            changed = false;
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
        return selection;
    }

    [[nodiscard]] std::expected<BackgroundEvaluationResult, std::string> run()
    {
        if (executed_) {
            return std::unexpected(
                "background evaluation transaction already executed");
        }
        executed_ = true;
        if (!graph_->background_operations.valid()) {
            return std::unexpected(
                "active graph has no complete background operations");
        }
        page_width_ = graph_->specialization.block_size;
        if (page_width_ == 0) {
            return std::unexpected(
                "background evaluation page width must be nonzero");
        }

        reader_ = pages_->register_reader();
        pin_.emplace(reader_.pin());
        auto const& base = pin_->snapshot();
        semantic_version_ = inputs_.semantic_version.value_or(
            base.version().semantic);
        if (semantic_version_ < base.version().semantic) {
            return std::unexpected(
                "background evaluation semantic version cannot move backwards");
        }
        if (base.page_width() != 0 &&
            base.page_width() != page_width_ &&
            (base.sample_page_count() != 0 || base.event_page_count() != 0)) {
            return std::unexpected(
                "persisted pages must be explicitly repaged before changing "
                "page width");
        }

        auto decoded = decode_produced_records();
        if (!decoded) return std::unexpected(std::move(decoded.error()));
        std::optional<PersistedPageStore::Candidate> candidate;
        if (!decoded->empty()) {
            candidate.emplace(pages_->begin_candidate(
                semantic_version_, page_width_));
            if (candidate->base_version() != base.version()) {
                return BackgroundEvaluationResult{
                    .status = BackgroundEvaluationStatus::stale_base,
                };
            }
            if (auto applied = apply_produced_records(
                    *decoded, base, *candidate); !applied) {
                return std::unexpected(std::move(applied.error()));
            }
        }
        working_pages_ = candidate
            ? &candidate->working_snapshot()
            : &base;
        prepared_.emplace(
            propagation_->prepare(graph_->background_operations, node_storage_,
                                  *coverage_, inputs_.roots,
                                  BackgroundCoverageDemandExpansion{
                                      .data = this,
                                      .expand_output = &expand_output_demand,
                                  }));

        auto binding = binding_selection();
        auto storage = storage_selection(binding, *working_pages_);
        auto realization = std::make_unique<BackgroundStorageRealization>(
            graph_->background_evaluation_plan, *pages_, std::move(storage));
        if (auto sealed = realization->seal(); !sealed) {
            return std::unexpected(std::move(sealed.error()));
        }

        BackgroundEvaluationCallFrame frame{graph_->background_evaluation_plan,
                                            *realization, binding};
        for (std::size_t node = 0; node < prepared_->node_activity().size();
             ++node) {
            frame.nodes()[node].activity = prepared_->node_activity()[node];
        }
        auto const& runtime = graph_->background_evaluation_plan.runtime;
        for (graph_jit::BackgroundReplayInvocationSlot slot = 0;
             slot < runtime.replay_invocations.size(); ++slot) {
            Coverage requested;
            auto const& replay = runtime.replay_invocations[slot];
            for (auto const binding_slot : replay.output_bindings) {
                requested.include(binding.binding_coverage[binding_slot]);
            }
            if (requested.empty()) {
                for (auto const binding_slot : replay.input_bindings) {
                    requested.include(binding.binding_coverage[binding_slot]);
                }
            }
            auto set = frame.set_replay_regions(
                slot, replay_regions(requested, replay.maximum_block_size));
            if (!set) return std::unexpected(std::move(set.error()));
        }
        if (auto sealed = frame.seal(); !sealed) {
            return std::unexpected(std::move(sealed.error()));
        }

        auto const& storage_selection = realization->selection();
        bool has_produced_pages = false;
        for (graph_jit::PortStorageIndex index = 0;
             index < graph_->background_evaluation_plan.storage.ports.size();
             ++index) {
            auto const& planned =
                graph_->background_evaluation_plan.storage.ports[index];
            if (planned.storage ==
                    graph_jit::PortStorageKind::persisted_pages &&
                storage_selection.produce_storage[index] &&
                !storage_selection.storage_coverage[index].empty()) {
                has_produced_pages = true;
                break;
            }
        }
        if (has_produced_pages || !page_mutations_.empty()) {
            if (!candidate) {
                candidate.emplace(pages_->begin_candidate(
                    semantic_version_, page_width_));
            }
            if (candidate->base_version() != pin_->snapshot().version()) {
                discard_prepared();
                return BackgroundEvaluationResult{
                    .status = BackgroundEvaluationStatus::stale_base,
                };
            }
            for (auto const& mutation : page_mutations_) {
                candidate->erase_page(mutation.output, mutation.page);
            }
            working_pages_ = &candidate->working_snapshot();
        }

        graph_->background_operations.evaluate(node_storage_, &frame.call());
        if (auto complete = frame.validate_evaluation(); !complete) {
            return std::unexpected(std::move(complete.error()));
        }

        std::unique_ptr<TickMaterializationSnapshot> tick_materialization;
        if (materializations_) {
            auto frozen = realization->make_tick_materialization_snapshot(
                graph_->project_generation,
                semantic_version_,
                candidate ? candidate->target_version() : base.version());
            if (!frozen) {
                return std::unexpected(std::move(frozen.error()));
            }
            tick_materialization = std::move(*frozen);
        }

        std::optional<PersistedPageSnapshotVersion> published;
        if (candidate) {
            if (auto staged = realization->stage_persisted_pages(*candidate);
                !staged) {
                return std::unexpected(std::move(staged.error()));
            }
            published = candidate->target_version();
            if (pages_->publish(std::move(*candidate)) ==
                PersistedPagePublishResult::stale_base) {
                discard_prepared();
                return BackgroundEvaluationResult{
                    .status = BackgroundEvaluationStatus::stale_base,
                };
            }
        } else if (!pages_->is_current(pin_->snapshot())) {
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
            .published_pages = published,
            .promoted_tick_materialization =
                promoted_tick_materialization,
        };
    }

public:
    Impl(CompiledGraph const& graph, std::byte* node_storage,
         BackgroundCoverageState& coverage,
         BackgroundPropagationWorkspace& propagation, PersistedPageStore& pages,
         TickMaterializationStore* materializations,
         BackgroundTransactionInputs inputs,
         std::span<BackgroundProducedInputRoute const> produced_routes = {},
         std::span<PinnedBlockPrefix const> produced_prefixes = {})
        : graph_(&graph)
        , node_storage_(node_storage)
        , coverage_(&coverage)
        , propagation_(&propagation)
        , pages_(&pages)
        , materializations_(materializations)
        , inputs_(std::move(inputs))
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

BackgroundEvaluationTransaction::BackgroundEvaluationTransaction(
    CompiledGraph const& graph, std::byte* node_storage,
    BackgroundCoverageState& coverage,
    BackgroundPropagationWorkspace& propagation, PersistedPageStore& pages,
    BackgroundTransactionInputs inputs)
    : impl_(std::make_unique<Impl>(graph, node_storage, coverage, propagation,
                                   pages, nullptr, std::move(inputs)))
{}

BackgroundEvaluationTransaction::BackgroundEvaluationTransaction(
    CompiledGraph const& graph, std::byte* node_storage,
    BackgroundCoverageState& coverage,
    BackgroundPropagationWorkspace& propagation, PersistedPageStore& pages,
    TickMaterializationStore& materializations,
    BackgroundTransactionInputs inputs)
    : impl_(std::make_unique<Impl>(
        graph,
        node_storage,
        coverage,
        propagation,
        pages,
        &materializations,
        std::move(inputs)))
{}

BackgroundEvaluationTransaction::BackgroundEvaluationTransaction(
    CompiledGraph const& graph, std::byte* node_storage,
    BackgroundCoverageState& coverage,
    BackgroundPropagationWorkspace& propagation, PersistedPageStore& pages,
    TickMaterializationStore& materializations,
    BackgroundTransactionInputs inputs,
    std::span<BackgroundProducedInputRoute const> produced_routes,
    std::span<PinnedBlockPrefix const> produced_prefixes)
    : impl_(std::make_unique<Impl>(
        graph,
        node_storage,
        coverage,
        propagation,
        pages,
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
