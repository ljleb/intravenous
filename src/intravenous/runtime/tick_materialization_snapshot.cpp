#include <intravenous/runtime/tick_materialization_snapshot.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace iv {
namespace {

[[nodiscard]] std::size_t checked_product(
    std::size_t left, std::size_t right)
{
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
        throw std::length_error(
            "Tick sample materialization is too large");
    }
    return left * right;
}

[[nodiscard]] bool event_matches_type(
    EventTypeId type, Event const& event) noexcept
{
    switch (type) {
    case EventTypeId::empty:
        return std::holds_alternative<EmptyEvent>(event);
    case EventTypeId::trigger:
        return std::holds_alternative<TriggerEvent>(event);
    case EventTypeId::boundary:
        return std::holds_alternative<BoundaryEvent>(event);
    case EventTypeId::midi:
        return std::holds_alternative<MidiEvent>(event);
    case EventTypeId::count:
        break;
    }
    return false;
}

} // namespace

bool TickMaterializedSampleInput::has_channel(
    std::size_t channel) const noexcept
{
    return std::ranges::contains(channels, channel);
}

std::size_t TickMaterializedSampleInput::channel_slot(
    std::size_t channel) const noexcept
{
    auto const found = std::ranges::lower_bound(channels, channel);
    return found != channels.end() && *found == channel
        ? static_cast<std::size_t>(std::distance(channels.begin(), found))
        : channels.size();
}

Sample TickMaterializedSampleInput::at(
    GlobalIndex index, std::size_t channel) const noexcept
{
    if (geometry.value_type != continuous_value_type_id_v<Sample>) return {};
    auto const slot = channel_slot(channel);
    if (slot == channels.size() || regions.empty()) return {};
    auto const found = std::ranges::find_if(
        regions, [&](TickMaterializedSampleRegion const& region) {
            return region.range.contains(index);
        });
    if (found == regions.end()) return {};
    auto const frame = found->first_frame + static_cast<std::size_t>(
        found->range.begin.forward_distance_to(index));
    auto const offset = layout.sample_layout == SampleStreamLayout::planar
        ? slot * found->storage_frames + frame
        : frame * channels.size() + slot;
    auto const* storage = reinterpret_cast<Sample const*>(found->storage_data());
    return storage ? storage[offset] : Sample{};
}

namespace {

void rebuild_merged_event_storage(TickMaterializedEventInput& input)
{
    struct Entry {
        TimedEvent event{};
        std::size_t semantic_source_index = 0;
        std::size_t region = 0;
        std::size_t position = 0;
    };
    std::vector<Entry> entries;
    auto event_count = std::size_t{};
    for (auto const& region : input.chunks) {
        if (region.end - region.begin
            > std::numeric_limits<std::size_t>::max() - event_count) {
            throw std::length_error(
                "Tick event materialization is too large");
        }
        event_count += region.end - region.begin;
    }
    entries.reserve(event_count);
    for (std::size_t region_index = 0;
         region_index < input.chunks.size(); ++region_index) {
        auto const& region = input.chunks[region_index];
        for (auto offset = region.begin; offset < region.end; ++offset) {
            entries.push_back(Entry{
                .event = (*region.events)[offset],
                .semantic_source_index =
                    (*region.semantic_source_indices)[offset],
                .region = region_index,
                .position = offset - region.begin,
            });
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](auto const& left,
                                                         auto const& right) {
        if (left.event.time != right.event.time) {
            return SignedGlobalIndexOrder{}(
                left.event.time, right.event.time);
        }
        if (left.semantic_source_index != right.semantic_source_index) {
            return left.semantic_source_index < right.semantic_source_index;
        }
        if (left.region != right.region) return left.region < right.region;
        return left.position < right.position;
    });
    auto events = std::make_shared<std::vector<TimedEvent>>();
    auto source_indices = std::make_shared<std::vector<std::size_t>>();
    events->reserve(entries.size());
    source_indices->reserve(entries.size());
    for (auto& entry : entries) {
        events->push_back(std::move(entry.event));
        source_indices->push_back(entry.semantic_source_index);
    }
    input.merged_events = std::move(events);
    input.merged_semantic_source_indices = std::move(source_indices);
}

template<typename Visit>
void for_each_merged_event(
    TickMaterializedEventInput const& input,
    GlobalIndexRange range,
    Visit&& visit)
{
    if (!input.merged_events || !input.merged_semantic_source_indices) return;
    range.for_each_signed_order_segment([&](GlobalIndexRange segment) {
        auto event = std::lower_bound(
            input.merged_events->begin(), input.merged_events->end(),
            segment.begin,
            [](TimedEvent const& value, GlobalIndex begin) {
                return SignedGlobalIndexOrder{}(value.time, begin);
            });
        for (; event != input.merged_events->end()
             && segment.contains(event->time); ++event) {
            auto const offset = static_cast<std::size_t>(
                std::distance(input.merged_events->begin(), event));
            visit(*event, (*input.merged_semantic_source_indices)[offset]);
        }
    });
}

template<typename Region>
std::vector<Region> merge_tick_materialization_regions(
    std::span<GlobalIndexRange const> logical_order,
    std::vector<Region> produced_regions,
    std::span<Region const> previous_regions,
    IndexCoverage const& produced,
    IndexCoverage const& retained)
{
    auto selected = produced | retained;
    if (!selected.empty() && logical_order.empty()) {
        throw std::invalid_argument(
            "Tick materialization retention lost the current logical Region order");
    }
    for (auto const& region : previous_regions) {
        auto const retained_part = IndexCoverage{region.range} & retained;
        auto const replaced_part = IndexCoverage{region.range} & produced;
        if (!retained_part.empty() && !retained.contains(region.range)) {
            throw std::invalid_argument(
                "Tick materialization retention clips a logical Region");
        }
        if (!replaced_part.empty() && !produced.contains(region.range)) {
            throw std::invalid_argument(
                "Tick materialization replacement clips a logical Region");
        }
    }

    std::vector<bool> used_produced(produced_regions.size(), false);
    std::vector<bool> used_previous(previous_regions.size(), false);
    std::vector<Region> result;
    result.reserve(produced_regions.size() + previous_regions.size());
    for (auto const expected : logical_order) {
        auto const relevant = IndexCoverage{expected} & selected;
        if (relevant.empty()) continue;
        if (!selected.contains(expected)) {
            throw std::invalid_argument(
                "Tick materialization selection clips a current logical Region");
        }

        auto replacement = produced_regions.size();
        for (std::size_t index = 0; index < produced_regions.size(); ++index) {
            if (!used_produced[index]
                && produced_regions[index].range == expected) {
                replacement = index;
                break;
            }
        }
        if (replacement != produced_regions.size()) {
            used_produced[replacement] = true;
            result.push_back(std::move(produced_regions[replacement]));
            continue;
        }

        auto previous = previous_regions.size();
        for (std::size_t index = 0; index < previous_regions.size(); ++index) {
            if (!used_previous[index]
                && previous_regions[index].range == expected
                && retained.contains(expected)) {
                previous = index;
                break;
            }
        }
        if (previous == previous_regions.size()) {
            throw std::invalid_argument(
                "Tick materialization cannot realize the current logical Region order");
        }
        used_previous[previous] = true;
        result.push_back(previous_regions[previous]);
    }

    if (std::ranges::contains(used_produced, false)) {
        throw std::invalid_argument(
            "Tick materialization produced a Region absent from the current logical order");
    }
    for (std::size_t index = 0; index < previous_regions.size(); ++index) {
        if (retained.contains(previous_regions[index].range)
            && !used_previous[index]) {
            throw std::invalid_argument(
                "Tick materialization retained a Region absent from the current logical order");
        }
    }
    return result;
}

} // namespace

void TickMaterializedEventInput::for_each(
    GlobalIndexRange range,
    void* visitor_data,
    RandomAccessEventInputPort::VisitEvent visitor) const
{
    if (visitor == nullptr) return;
    for_each_merged_event(*this, range,
        [&](TimedEvent const& event, std::size_t) {
        visitor(visitor_data, event);
    });
}

void TickMaterializedEventInput::for_each_indexed(
    GlobalIndexRange range,
    void* visitor_data,
    VisitIndexedEvent visitor) const
{
    if (visitor == nullptr) return;
    for_each_merged_event(*this, range,
        [&](TimedEvent const& event, std::size_t source_index) {
        visitor(visitor_data, event, source_index);
    });
}

void TickMaterializedEventInput::for_each_region(
    std::size_t region,
    GlobalIndexRange range,
    void* visitor_data,
    RandomAccessEventInputPort::VisitEvent visitor) const
{
    if (visitor == nullptr || region >= chunks.size()
        || !chunks[region].range.contains(range)) {
        return;
    }
    auto const& chunk = chunks[region];
    auto const first = chunk.events->begin()
        + static_cast<std::ptrdiff_t>(chunk.begin);
    auto const last = chunk.events->begin()
        + static_cast<std::ptrdiff_t>(chunk.end);
    for (auto event = first; event != last; ++event) {
        if (range.contains(event->time)) visitor(visitor_data, *event);
    }
}

TickMaterializedSampleInput make_tick_materialized_sample_input(
    graph_jit::BackgroundPortIndex port,
    ChannelLayout layout,
    std::vector<std::size_t> channels,
    std::vector<GlobalIndexRange> region_ranges,
    std::vector<Sample> values)
{
    IndexCoverage coverage;
    std::size_t frames = 0;
    for (auto const region : region_ranges) {
        if (region.empty()
            || region.extent > std::numeric_limits<std::size_t>::max()
            || static_cast<std::size_t>(region.extent)
                > std::numeric_limits<std::size_t>::max() - frames) {
            throw std::invalid_argument(
                "Tick sample materialization has an invalid Region");
        }
        frames += static_cast<std::size_t>(region.extent);
        coverage.include(region);
    }
    if (values.size() != checked_product(frames, channels.size())) {
        throw std::invalid_argument(
            "Tick sample materialization values disagree with their coverage");
    }
    TickMaterializedSampleInput result{
        .port = port,
        .layout = layout,
        .geometry = *continuous_value_storage_geometry(
            continuous_value_type_id_v<Sample>, std::nullopt),
        .channels = std::move(channels),
    };
    std::size_t value_cursor = 0;
    for (auto const region : region_ranges) {
        auto const region_frames = static_cast<std::size_t>(region.extent);
        auto const value_count =
            checked_product(region_frames, result.channels.size());
        auto storage = std::make_shared<std::vector<Sample> const>(
            values.begin() + static_cast<std::ptrdiff_t>(value_cursor),
            values.begin() + static_cast<std::ptrdiff_t>(
                value_cursor + value_count));
        result.regions.push_back(TickMaterializedSampleRegion{
            .range = region,
            .values = storage,
            .storage_frames = region_frames,
            .first_frame = 0,
        });
        result.region_views.push_back(ContinuousRegionStorageView{
            .storage = reinterpret_cast<std::byte*>(
                const_cast<Sample*>(storage->data())),
            .range = region,
            .geometry = *continuous_value_storage_geometry(
                continuous_value_type_id_v<Sample>, std::nullopt),
            .channel_layout = layout,
        });
        value_cursor += value_count;
    }
    result.coverage = std::move(coverage);
    return result;
}

TickMaterializedSampleInput make_tick_materialized_continuous_input(
    graph_jit::BackgroundPortIndex port,
    ChannelLayout layout,
    ContinuousValueStorageGeometry geometry,
    std::vector<std::size_t> channels,
    std::vector<GlobalIndexRange> region_ranges,
    std::vector<std::byte> values)
{
    if (geometry.value_size_bytes == 0
        || geometry.element_alignment == 0
        || !std::has_single_bit(geometry.element_alignment)) {
        throw std::invalid_argument(
            "Tick continuous materialization has invalid geometry");
    }
    IndexCoverage coverage;
    std::size_t value_count = 0;
    for (auto const region : region_ranges) {
        if (region.empty()
            || region.extent > std::numeric_limits<std::size_t>::max()
            || static_cast<std::size_t>(region.extent)
                > std::numeric_limits<std::size_t>::max() - value_count) {
            throw std::invalid_argument(
                "Tick continuous materialization has an invalid Region");
        }
        value_count += static_cast<std::size_t>(region.extent);
        coverage.include(region);
    }
    auto const expected = checked_product(
        checked_product(value_count, channels.size()),
        geometry.value_size_bytes);
    if (values.size() != expected) {
        throw std::invalid_argument(
            "Tick continuous materialization values disagree with their Regions");
    }
    TickMaterializedSampleInput result{
        .port = port,
        .layout = layout,
        .geometry = geometry,
        .channels = std::move(channels),
    };
    std::size_t byte_cursor = 0;
    for (auto const region : region_ranges) {
        auto const region_frames = static_cast<std::size_t>(region.extent);
        auto const byte_count = checked_product(
            checked_product(region_frames, result.channels.size()),
            geometry.value_size_bytes);
        auto storage = std::make_shared<PersistedContinuousStorage>(
            byte_count, geometry.element_alignment);
        std::memcpy(
            storage->data(), values.data() + byte_cursor, byte_count);
        result.regions.push_back(TickMaterializedSampleRegion{
            .range = region,
            .continuous_values = storage,
            .storage_frames = region_frames,
            .first_frame = 0,
        });
        result.region_views.push_back(ContinuousRegionStorageView{
            .storage = storage->data(),
            .range = region,
            .geometry = geometry,
            .channel_layout = layout,
        });
        byte_cursor += byte_count;
    }
    result.coverage = std::move(coverage);
    return result;
}

TickMaterializedSampleInput make_tick_materialized_sample_input(
    graph_jit::BackgroundPortIndex port,
    ChannelLayout layout,
    std::vector<std::size_t> channels,
    IndexCoverage coverage,
    std::vector<Sample> values)
{
    std::size_t total_frames = 0;
    for (auto const region : coverage) {
        if (region.extent > std::numeric_limits<std::size_t>::max()
            || static_cast<std::size_t>(region.extent)
                > std::numeric_limits<std::size_t>::max() - total_frames) {
            throw std::invalid_argument(
                "Tick sample materialization has an invalid coverage");
        }
        total_frames += static_cast<std::size_t>(region.extent);
    }
    if (values.size() != checked_product(total_frames, channels.size())) {
        throw std::invalid_argument(
            "Tick sample materialization values disagree with their coverage");
    }
    std::vector<Sample> region_major;
    region_major.reserve(values.size());
    std::size_t frame_offset = 0;
    for (auto const region : coverage) {
        auto const frames = static_cast<std::size_t>(region.extent);
        if (layout.sample_layout == SampleStreamLayout::planar) {
            for (std::size_t channel = 0; channel < channels.size(); ++channel) {
                auto const begin = values.begin()
                    + static_cast<std::ptrdiff_t>(
                        channel * total_frames + frame_offset);
                region_major.insert(
                    region_major.end(), begin,
                    begin + static_cast<std::ptrdiff_t>(frames));
            }
        } else {
            for (std::size_t frame = 0; frame < frames; ++frame) {
                for (std::size_t channel = 0; channel < channels.size(); ++channel) {
                    region_major.push_back(
                        values[channel * total_frames + frame_offset + frame]);
                }
            }
        }
        frame_offset += frames;
    }
    return make_tick_materialized_sample_input(
        port,
        layout,
        std::move(channels),
        std::vector<GlobalIndexRange>{coverage.begin(), coverage.end()},
        std::move(region_major));
}

TickMaterializedEventInput make_tick_materialized_event_input(
    graph_jit::BackgroundPortIndex port,
    EventTypeId type,
    IndexCoverage coverage,
    std::vector<TimedEvent> events,
    std::vector<std::size_t> semantic_source_indices)
{
    if (semantic_source_indices.empty()) {
        semantic_source_indices.resize(
            events.size(), std::numeric_limits<std::size_t>::max());
    }
    if (semantic_source_indices.size() != events.size()) {
        throw std::invalid_argument(
            "Tick event materialization source indices disagree with its events");
    }
    auto shared_events = std::make_shared<std::vector<TimedEvent> const>(
        std::move(events));
    auto shared_indices = std::make_shared<std::vector<std::size_t> const>(
        std::move(semantic_source_indices));
    TickMaterializedEventInput result{
        .port = port,
        .type = type,
        .merged_events = shared_events,
        .merged_semantic_source_indices = shared_indices,
    };
    std::size_t cursor = 0;
    for (auto const region : coverage) {
        auto const begin = cursor;
        while (cursor < shared_events->size()
            && region.contains((*shared_events)[cursor].time)) {
            ++cursor;
        }
        result.chunks.push_back(TickMaterializedEventChunk{
            .range = region,
            .events = shared_events,
            .semantic_source_indices = shared_indices,
            .begin = begin,
            .end = cursor,
        });
        result.region_ranges.push_back(region);
    }
    if (cursor != shared_events->size()) {
        throw std::invalid_argument(
            "Tick event materialization events lie outside their coverage");
    }
    result.coverage = std::move(coverage);
    return result;
}

TickMaterializedEventInput make_tick_materialized_event_input(
    graph_jit::BackgroundPortIndex port,
    EventTypeId type,
    std::vector<GlobalIndexRange> regions,
    std::vector<std::vector<TimedEvent>> events,
    std::vector<std::vector<std::size_t>> semantic_source_indices)
{
    if (events.size() != regions.size()
        || (!semantic_source_indices.empty()
            && semantic_source_indices.size() != regions.size())) {
        throw std::invalid_argument(
            "Tick event materialization Region payloads disagree with their Region list");
    }
    if (semantic_source_indices.empty()) {
        semantic_source_indices.resize(regions.size());
    }
    TickMaterializedEventInput result{
        .port = port,
        .type = type,
        .region_ranges = regions,
    };
    for (std::size_t region = 0; region < regions.size(); ++region) {
        if (regions[region].empty()) {
            throw std::invalid_argument(
                "Tick event materialization contains an empty Region");
        }
        auto& indices = semantic_source_indices[region];
        if (indices.empty()) {
            indices.resize(
                events[region].size(),
                std::numeric_limits<std::size_t>::max());
        }
        if (indices.size() != events[region].size()
            || std::ranges::any_of(events[region], [&](TimedEvent const& event) {
                return !regions[region].contains(event.time);
            })
            || !std::ranges::is_sorted(
                events[region],
                RangeRelativeIndexOrder{regions[region].begin},
                &TimedEvent::time)) {
            throw std::invalid_argument(
                "Tick event materialization has an invalid Region payload");
        }
        auto shared_events =
            std::make_shared<std::vector<TimedEvent> const>(
                std::move(events[region]));
        auto shared_indices =
            std::make_shared<std::vector<std::size_t> const>(
                std::move(indices));
        result.chunks.push_back(TickMaterializedEventChunk{
            .range = regions[region],
            .events = std::move(shared_events),
            .semantic_source_indices = std::move(shared_indices),
            .begin = 0,
            .end = 0,
        });
        result.chunks.back().end = result.chunks.back().events->size();
        result.coverage.include(regions[region]);
    }
    rebuild_merged_event_storage(result);
    return result;
}

TickMaterializationSnapshot::TickMaterializationSnapshot(
    std::uint64_t generation,
    std::uint64_t semantic_version,
    PersistedDataVersion data_version,
    std::vector<TickMaterializedSampleInput> samples,
    std::vector<TickMaterializedEventInput> events)
    : generation_(generation)
    , semantic_version_(semantic_version)
    , data_version_(data_version)
    , samples_(std::move(samples))
    , events_(std::move(events))
{
    validate();
}

void TickMaterializationSnapshot::validate() const
{
    std::vector<graph_jit::BackgroundPortIndex> ports;
    ports.reserve(samples_.size() + events_.size());
    for (auto const& sample : samples_) {
        IndexCoverage covered;
        bool valid = sample.region_views.size() == sample.regions.size()
            && is_registered_continuous_value_type(sample.geometry.value_type)
            && sample.geometry.value_size_bytes != 0
            && is_valid_channel_type(sample.layout.channel_type)
            && is_valid_sample_stream_layout(sample.layout.sample_layout)
            && sample.channels.size() == channel_count(sample.layout)
            && std::ranges::is_sorted(sample.channels)
            && std::ranges::adjacent_find(sample.channels)
                == sample.channels.end()
            && std::ranges::none_of(
                sample.channels,
                [&](std::size_t channel) {
                    return channel >= channel_count(sample.layout);
                });
        for (std::size_t index = 0;
             valid && index < sample.regions.size(); ++index) {
            auto const& region = sample.regions[index];
            if (region.range.extent
                > std::numeric_limits<std::size_t>::max()) {
                valid = false;
                break;
            }
            auto const extent = static_cast<std::size_t>(region.range.extent);
            auto const stored_values = checked_product(
                region.storage_frames, sample.channels.size());
            auto const sample_owner = region.values != nullptr
                && region.continuous_values == nullptr
                && sample.geometry.value_type
                    == continuous_value_type_id_v<Sample>
                && region.values->size() == stored_values;
            auto const continuous_owner = region.values == nullptr
                && region.continuous_values != nullptr
                && region.continuous_values->size()
                    == checked_product(
                        stored_values, sample.geometry.value_size_bytes)
                && region.continuous_values->alignment()
                    == sample.geometry.element_alignment;
            valid = !region.range.empty()
                && (sample_owner || continuous_owner)
                && region.first_frame <= region.storage_frames
                && extent <= region.storage_frames - region.first_frame
                && index < sample.region_views.size()
                && sample.region_views[index].range == region.range
                && sample.region_views[index].channel_layout == sample.layout
                && sample.region_views[index].geometry == sample.geometry
                && sample.region_views[index].storage
                    == const_cast<std::byte*>(region.storage_data());
            covered.include(region.range);
        }
        if (!valid || covered != sample.coverage) {
            throw std::invalid_argument(
                "Tick sample materialization is inconsistent");
        }
        ports.push_back(sample.port);
    }
    for (auto const& event : events_) {
        IndexCoverage covered;
        bool valid = event.chunks.size() == event.region_ranges.size()
            && event.type < EventTypeId::count
            && event.merged_events != nullptr
            && event.merged_semantic_source_indices != nullptr
            && event.merged_events->size()
                == event.merged_semantic_source_indices->size();
        auto event_count = std::size_t{};
        for (std::size_t index = 0;
             valid && index < event.chunks.size(); ++index) {
            auto const& chunk = event.chunks[index];
            valid = !chunk.range.empty() && chunk.events != nullptr
                && chunk.semantic_source_indices != nullptr
                && chunk.semantic_source_indices->size()
                    == chunk.events->size()
                && chunk.begin <= chunk.end
                && chunk.end <= chunk.events->size()
                && chunk.range == event.region_ranges[index];
            if (!valid
                || chunk.end - chunk.begin
                    > std::numeric_limits<std::size_t>::max()
                        - event_count) {
                valid = false;
                break;
            }
            event_count += chunk.end - chunk.begin;
            for (auto offset = chunk.begin; valid && offset < chunk.end;
                 ++offset) {
                auto const& value = (*chunk.events)[offset];
                valid = chunk.range.contains(value.time)
                    && event_matches_type(event.type, value.value)
                    && (offset == chunk.begin
                        || !RangeRelativeIndexOrder{chunk.range.begin}(
                            value.time, (*chunk.events)[offset - 1].time));
            }
            covered.include(chunk.range);
        }
        for (std::size_t index = 0;
             valid && index < event.merged_events->size(); ++index) {
            auto const& value = (*event.merged_events)[index];
            valid = event.coverage.contains(value.time)
                && event_matches_type(event.type, value.value)
                && (index == 0
                    || !SignedGlobalIndexOrder{}(
                        value.time, (*event.merged_events)[index - 1].time));
        }
        if (!valid || event_count != event.merged_events->size()
            || covered != event.coverage) {
            throw std::invalid_argument(
                "Tick event materialization is inconsistent");
        }
        ports.push_back(event.port);
    }
    std::ranges::sort(ports);
    if (std::ranges::adjacent_find(ports) != ports.end()) {
        throw std::invalid_argument(
            "Tick materialization input port is duplicated");
    }
}

void TickMaterializationSnapshot::carry_forward(
    TickMaterializationSnapshot const& previous,
    std::span<TickMaterializationRetention const> retention)
{
    if (previous.generation_ != generation_) {
        throw std::invalid_argument(
            "Tick materialization can only carry forward within one generation");
    }
    for (auto const& retained : retention) {
        if (retained.retained.empty()) continue;
        if (auto const* source = previous.find_sample(retained.port)) {
            // This snapshot's own materialization supersedes carried data.
            IndexCoverage produced;
            if (auto const* own = find_sample(retained.port)) {
                produced = own->coverage;
            }
            auto target = std::ranges::find(
                samples_, retained.port, &TickMaterializedSampleInput::port);
            if (target == samples_.end()) {
                samples_.push_back(TickMaterializedSampleInput{
                    .port = source->port,
                    .layout = source->layout,
                    .geometry = source->geometry,
                    .channels = source->channels,
                });
                target = std::prev(samples_.end());
            } else if (target->layout != source->layout
                || target->geometry != source->geometry
                || target->channels != source->channels) {
                throw std::invalid_argument(
                    "carried Tick sample materialization changed its layout");
            }

            target->regions = merge_tick_materialization_regions(
                std::span<GlobalIndexRange const>{retained.logical_regions},
                std::move(target->regions),
                std::span<TickMaterializedSampleRegion const>{source->regions},
                produced,
                retained.retained);
            target->region_views.clear();
            target->coverage = {};
            target->region_views.reserve(target->regions.size());
            for (auto const& region : target->regions) {
                target->coverage.include(region.range);
                target->region_views.push_back(ContinuousRegionStorageView{
                    .storage = const_cast<std::byte*>(region.storage_data()),
                    .range = region.range,
                    .geometry = target->geometry,
                    .channel_layout = target->layout,
                });
            }
        }
        if (auto const* source = previous.find_event(retained.port)) {
            IndexCoverage produced;
            if (auto const* own = find_event(retained.port)) {
                produced = own->coverage;
            }
            auto target = std::ranges::find(
                events_, retained.port, &TickMaterializedEventInput::port);
            if (target == events_.end()) {
                events_.push_back(TickMaterializedEventInput{
                    .port = source->port,
                    .type = source->type,
                });
                target = std::prev(events_.end());
            } else if (target->type != source->type) {
                throw std::invalid_argument(
                    "carried Tick event materialization changed its type");
            }

            target->chunks = merge_tick_materialization_regions(
                std::span<GlobalIndexRange const>{retained.logical_regions},
                std::move(target->chunks),
                std::span<TickMaterializedEventChunk const>{source->chunks},
                produced,
                retained.retained);
            target->region_ranges.clear();
            target->coverage = {};
            target->region_ranges.reserve(target->chunks.size());
            for (auto const& region : target->chunks) {
                target->region_ranges.push_back(region.range);
                target->coverage.include(region.range);
            }
            rebuild_merged_event_storage(*target);
        }
    }
    validate();
}

TickMaterializedSampleInput const* TickMaterializationSnapshot::find_sample(
    graph_jit::BackgroundPortIndex port) const noexcept
{
    auto const found = std::ranges::find(
        samples_, port, &TickMaterializedSampleInput::port);
    return found == samples_.end() ? nullptr : &*found;
}

TickMaterializedEventInput const* TickMaterializationSnapshot::find_event(
    graph_jit::BackgroundPortIndex port) const noexcept
{
    auto const found = std::ranges::find(
        events_, port, &TickMaterializedEventInput::port);
    return found == events_.end() ? nullptr : &*found;
}

TickMaterializationStore::ReaderPin::ReaderPin(
    ReaderSlotState& slot,
    std::atomic<TickMaterializationSnapshot const*> const& published) noexcept
    : slot_(&slot)
{
    assert(slot.pinned.load(std::memory_order_seq_cst) == nullptr);
    slot.acquiring.store(true, std::memory_order_seq_cst);
    snapshot_ = published.load(std::memory_order_seq_cst);
    slot.pinned.store(snapshot_, std::memory_order_seq_cst);
    slot.acquiring.store(false, std::memory_order_seq_cst);
}

void TickMaterializationStore::ReaderPin::reset() noexcept
{
    if (!slot_) return;
    slot_->pinned.store(nullptr, std::memory_order_seq_cst);
    slot_ = nullptr;
    snapshot_ = nullptr;
}

TickMaterializationStore::ReaderPin::~ReaderPin()
{
    reset();
}

TickMaterializationStore::ReaderPin::ReaderPin(ReaderPin&& other) noexcept
    : slot_(std::exchange(other.slot_, nullptr))
    , snapshot_(std::exchange(other.snapshot_, nullptr))
{}

TickMaterializationStore::ReaderPin&
TickMaterializationStore::ReaderPin::operator=(ReaderPin&& other) noexcept
{
    if (this == &other) return *this;
    reset();
    slot_ = std::exchange(other.slot_, nullptr);
    snapshot_ = std::exchange(other.snapshot_, nullptr);
    return *this;
}

TickMaterializationSnapshot const&
TickMaterializationStore::ReaderPin::snapshot() const noexcept
{
    assert(snapshot_ != nullptr);
    return *snapshot_;
}

TickMaterializationStore::ReaderSlot::ReaderSlot(
    TickMaterializationStore& store,
    ReaderSlotState& state) noexcept
    : store_(&store)
    , state_(&state)
{}

void TickMaterializationStore::ReaderSlot::reset() noexcept
{
    if (!state_) return;
    store_->unregister_reader(*state_);
    store_ = nullptr;
    state_ = nullptr;
}

TickMaterializationStore::ReaderSlot::~ReaderSlot()
{
    reset();
}

TickMaterializationStore::ReaderSlot::ReaderSlot(ReaderSlot&& other) noexcept
    : store_(std::exchange(other.store_, nullptr))
    , state_(std::exchange(other.state_, nullptr))
{}

TickMaterializationStore::ReaderSlot&
TickMaterializationStore::ReaderSlot::operator=(ReaderSlot&& other) noexcept
{
    if (this == &other) return *this;
    reset();
    store_ = std::exchange(other.store_, nullptr);
    state_ = std::exchange(other.state_, nullptr);
    return *this;
}

TickMaterializationStore::ReaderPin
TickMaterializationStore::ReaderSlot::pin() noexcept
{
    assert(store_ != nullptr && state_ != nullptr);
    return ReaderPin{*state_, store_->published_};
}

TickMaterializationStore::TickMaterializationStore()
    : published_owner_(std::make_unique<TickMaterializationSnapshot>())
    , published_(published_owner_.get())
{}

TickMaterializationStore::~TickMaterializationStore()
{
    assert(reader_slots_.empty());
    while (retired_) {
        auto removed = std::move(retired_);
        retired_ = std::move(removed->retired_next_);
    }
}

std::uint64_t TickMaterializationStore::promote(
    std::unique_ptr<TickMaterializationSnapshot> snapshot) noexcept
{
    assert(snapshot != nullptr);
    if (!snapshot) return promotion_sequence_;
    if (promotion_sequence_ != std::numeric_limits<std::uint64_t>::max()) {
        ++promotion_sequence_;
    }
    snapshot->promotion_sequence_ = promotion_sequence_;
    published_owner_->retired_next_ = std::move(retired_);
    retired_ = std::move(published_owner_);
    published_owner_ = std::move(snapshot);
    published_.store(published_owner_.get(), std::memory_order_seq_cst);
    return promotion_sequence_;
}

TickMaterializationStore::ReaderSlot
TickMaterializationStore::register_reader()
{
    auto state = std::make_unique<ReaderSlotState>();
    auto* pointer = state.get();
    std::lock_guard lock{reader_slots_mutex_};
    reader_slots_.push_back(std::move(state));
    return ReaderSlot{*this, *pointer};
}

void TickMaterializationStore::unregister_reader(
    ReaderSlotState& state) noexcept
{
    std::lock_guard lock{reader_slots_mutex_};
    assert(!state.acquiring.load(std::memory_order_seq_cst));
    assert(state.pinned.load(std::memory_order_seq_cst) == nullptr);
    auto const found = std::ranges::find_if(
        reader_slots_, [&](auto const& candidate) {
            return candidate.get() == &state;
        });
    assert(found != reader_slots_.end());
    if (found != reader_slots_.end()) reader_slots_.erase(found);
}

std::size_t TickMaterializationStore::reclaim_retired()
{
    std::lock_guard lock{reader_slots_mutex_};
    if (std::ranges::any_of(reader_slots_, [](auto const& slot) {
            return slot->acquiring.load(std::memory_order_seq_cst);
        })) {
        return 0;
    }
    std::size_t reclaimed = 0;
    auto* link = &retired_;
    while (*link) {
        auto const pinned = std::ranges::any_of(
            reader_slots_, [&](auto const& slot) {
                return slot->pinned.load(std::memory_order_seq_cst)
                    == link->get();
            });
        if (pinned) {
            link = &(*link)->retired_next_;
            continue;
        }
        auto removed = std::move(*link);
        *link = std::move(removed->retired_next_);
        ++reclaimed;
    }
    return reclaimed;
}

std::size_t TickMaterializationStore::retired_snapshot_count() const noexcept
{
    std::size_t count = 0;
    for (auto const* snapshot = retired_.get(); snapshot != nullptr;
         snapshot = snapshot->retired_next_.get()) {
        ++count;
    }
    return count;
}

} // namespace iv
