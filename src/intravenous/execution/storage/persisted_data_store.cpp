#include <intravenous/execution/storage/persisted_data_store.h>
#include <intravenous/execution/storage/buffered_storage.h>
#include <intravenous/execution/playback_preparation_metrics.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <limits>
#include <iterator>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace iv {
namespace persisted_data_store_detail {

template<typename Record>
struct RangeNode {
    std::shared_ptr<Record const> record{};
    std::shared_ptr<RangeNode const> left{};
    std::shared_ptr<RangeNode const> right{};
    std::uint32_t height = 1;
};

struct BufferedPageKey {
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    auto operator<=>(BufferedPageKey const&) const = default;
};


struct BufferedPageNode {
    BufferedPageKey key{};
    std::shared_ptr<BufferedPage const> page{};
    std::shared_ptr<BufferedPageNode const> left{};
    std::shared_ptr<BufferedPageNode const> right{};
    std::uint32_t height = 1;
};

struct BufferedPageRun {
    BufferedPageKey first{};
    std::size_t page_count = 0;
    std::uint64_t file_offset = 0;
};

struct BufferedPageRunNode {
    BufferedPageRun run{};
    std::shared_ptr<BufferedPageRunNode const> left{};
    std::shared_ptr<BufferedPageRunNode const> right{};
    std::uint32_t height = 1;
};


struct SampleOutputState {
    std::shared_ptr<void const> semantic_identity = std::make_shared<std::byte>();
    std::shared_ptr<PersistedDataStore::Reconstruction const> reconstruction{};
    PersistedOutputHandle output{};
    IndexCoverage coverage{};
    std::optional<IndexCoverage> logical_coverage{};
    ChannelLayout layout{};
    ContinuousValueStorageGeometry geometry{};
    std::shared_ptr<RangeNode<PersistedSampleRegion> const> regions{};
    std::shared_ptr<std::vector<PersistedSampleRegion> const> logical_regions{};
    std::shared_ptr<std::vector<ContinuousRegionStorageView> const> region_views{};
    std::shared_ptr<BufferedPageAllocator> recording_allocator{};
    std::shared_ptr<BufferedPageNode const> buffered_pages{};
    std::shared_ptr<BufferedPageRunNode const> recording_runs{};
    bool positional_recording = false;
    std::size_t physical_region_count = 0;
    std::size_t region_count = 0;
};

struct BufferedSampleBacking {
    std::shared_ptr<BufferedPageAllocator> allocator{};
    std::shared_ptr<BufferedPageNode const> pages{};
    std::shared_ptr<BufferedPageRunNode const> runs{};
    GlobalIndex origin{};
    unsigned __int128 byte_begin = 0;
    // Authored Regions own a stable native-coordinate directory. Planar
    // channel origins are page aligned independently of prepared windows.
    std::optional<GlobalIndexRange> logical_range{};
    unsigned __int128 plane_stride_bytes = 0;
};

struct EventOutputState {
    std::shared_ptr<void const> semantic_identity = std::make_shared<std::byte>();
    std::shared_ptr<PersistedDataStore::Reconstruction const> reconstruction{};
    PersistedOutputHandle output{};
    IndexCoverage coverage{};
    std::optional<IndexCoverage> logical_coverage{};
    EventTypeId type = EventTypeId::empty;
    std::shared_ptr<RangeNode<PersistedEventChunk> const> chunks{};
    std::shared_ptr<std::vector<PersistedEventChunk> const> logical_regions{};
    std::shared_ptr<std::vector<GlobalIndexRange> const> region_ranges{};
    std::shared_ptr<std::vector<TimedEvent> const> timeline{};
    bool positional_recording = false;
    std::size_t chunk_count = 0;
    std::shared_ptr<void const> timeline_accounting{};
};

template<typename State>
struct OutputDirectoryNode {
    using Node = OutputDirectoryNode<State>;
    using Children = std::array<std::shared_ptr<Node const>, 16>;
    using Values = std::array<std::shared_ptr<State const>, 16>;

    std::variant<Children, Values> entries{Values{}};
};

} // namespace persisted_data_store_detail


namespace {

using persisted_data_store_detail::EventOutputNode;
using persisted_data_store_detail::EventOutputState;
using persisted_data_store_detail::RangeNode;
using persisted_data_store_detail::BufferedPage;
using persisted_data_store_detail::BufferedPageAllocator;
using persisted_data_store_detail::BufferedSampleBacking;
using persisted_data_store_detail::BufferedPageKey;
using persisted_data_store_detail::BufferedPageNode;
using persisted_data_store_detail::BufferedPageRun;
using persisted_data_store_detail::BufferedPageRunNode;
using persisted_data_store_detail::SampleOutputNode;
using persisted_data_store_detail::SampleOutputState;

[[nodiscard]] std::shared_ptr<std::vector<GlobalIndexRange> const>
make_event_region_ranges(
    std::shared_ptr<std::vector<PersistedEventChunk> const> const& regions)
{
    auto ranges = std::make_shared<std::vector<GlobalIndexRange>>();
    if (regions) {
        ranges->reserve(regions->size());
        for (auto const& region : *regions) ranges->push_back(region.range);
    }
    return std::shared_ptr<std::vector<GlobalIndexRange> const>{
        std::move(ranges)};
}

[[nodiscard]] std::shared_ptr<std::vector<GlobalIndexRange> const>
make_event_region_ranges(IndexCoverage const& coverage)
{
    auto ranges = std::make_shared<std::vector<GlobalIndexRange>>(
        coverage.begin(), coverage.end());
    return std::shared_ptr<std::vector<GlobalIndexRange> const>{
        std::move(ranges)};
}

[[nodiscard]] std::shared_ptr<std::vector<TimedEvent> const>
make_event_timeline(
    std::shared_ptr<std::vector<PersistedEventChunk> const> const& regions)
{
    auto timeline = std::make_shared<std::vector<TimedEvent>>();
    if (regions) {
        auto count = std::size_t{};
        for (auto const& region : *regions) {
            if (region.events.size()
                > std::numeric_limits<std::size_t>::max() - count) {
                throw std::length_error(
                    "persisted event timeline is too large");
            }
            count += region.events.size();
        }
        timeline->reserve(count);
        for (auto const& region : *regions) {
            timeline->insert(
                timeline->end(), region.events.begin(), region.events.end());
        }
        std::stable_sort(
            timeline->begin(), timeline->end(),
            [](TimedEvent const& left, TimedEvent const& right) {
                return SignedGlobalIndexOrder{}(left.time, right.time);
            });
    }
    return std::shared_ptr<std::vector<TimedEvent> const>{
        std::move(timeline)};
}

std::atomic<std::uint32_t> next_persisted_output_owner{0};

[[nodiscard]] std::uint32_t allocate_persisted_output_owner()
{
    constexpr auto exclusive_limit =
        std::numeric_limits<std::uint32_t>::max() / 2;
    auto owner = next_persisted_output_owner.load(std::memory_order_relaxed);
    for (;;) {
        if (owner >= exclusive_limit) {
            throw std::overflow_error(
                "persisted data store owner tokens are exhausted");
        }
        if (next_persisted_output_owner.compare_exchange_weak(
                owner,
                owner + 1,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return owner;
        }
    }
}

[[nodiscard]] GlobalIndex from_signed_key(
    GlobalIndex::storage_type key) noexcept
{
    constexpr auto sign = GlobalIndex::storage_type{1} << 63;
    return GlobalIndex::from_bits(key ^ sign);
}

struct LinearBoundary {
    GlobalIndex::storage_type key = 0;
    bool domain_end = false;
};

struct LinearRange {
    GlobalIndex::storage_type begin = 0;
    LinearBoundary end{};
};

[[nodiscard]] constexpr bool less(
    GlobalIndex::storage_type left,
    LinearBoundary right) noexcept
{
    return right.domain_end || left < right.key;
}

[[nodiscard]] constexpr bool less(
    LinearBoundary left,
    LinearBoundary right) noexcept
{
    if (left.domain_end != right.domain_end) return !left.domain_end;
    return !left.domain_end && left.key < right.key;
}

[[nodiscard]] LinearRange linear_range(GlobalIndexRange range) noexcept
{
    assert(!range.empty());
    auto const begin = SignedGlobalIndexOrder::key(range.begin);
    if (begin == 0) {
        return {
            .begin = begin,
            .end = LinearBoundary{.key = range.extent},
        };
    }
    auto const until_end = GlobalIndex::extent_type{0} - begin;
    assert(range.extent <= until_end);
    if (range.extent == until_end) {
        return {
            .begin = begin,
            .end = LinearBoundary{.domain_end = true},
        };
    }
    return {
        .begin = begin,
        .end = LinearBoundary{.key = begin + range.extent},
    };
}

[[nodiscard]] GlobalIndexRange intersection(
    GlobalIndexRange left,
    GlobalIndexRange right) noexcept
{
    auto const a = linear_range(left);
    auto const b = linear_range(right);
    auto const begin = std::max(a.begin, b.begin);
    auto const end = less(a.end, b.end) ? a.end : b.end;
    if (!less(begin, end)) return {};
    return {
        .begin = from_signed_key(begin),
        .extent = end.domain_end
            ? GlobalIndex::extent_type{0} - begin
            : end.key - begin,
    };
}

[[nodiscard]] std::size_t checked_frame_count(GlobalIndexRange range)
{
    if (range.empty()
        || range.extent > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(
            "persisted data range has an unrepresentable frame count");
    }
    return static_cast<std::size_t>(range.extent);
}

[[nodiscard]] std::size_t checked_product(
    std::size_t left,
    std::size_t right)
{
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
        throw std::invalid_argument(
            "persisted sample region payload is too large");
    }
    return left * right;
}

[[nodiscard]] bool event_matches_type(
    EventTypeId type,
    Event const& event) noexcept
{
    switch (type) {
    case EventTypeId::empty:
        return std::holds_alternative<EmptyEvent>(event);
    case EventTypeId::trigger:
        return std::holds_alternative<TriggerEvent>(event);
    case EventTypeId::boundary:
        return std::holds_alternative<BoundaryEvent>(event);
    case EventTypeId::sample:
        return std::holds_alternative<SampleEvent>(event);
    case EventTypeId::curve_point:
        return std::holds_alternative<CurvePointEvent>(event);
    case EventTypeId::beat:
        return std::holds_alternative<BeatEvent>(event);
    case EventTypeId::midi:
        return std::holds_alternative<MidiEvent>(event);
    case EventTypeId::count:
        break;
    }
    return false;
}

void validate_sample_representation(ChannelLayout layout, ContinuousValueStorageGeometry geometry)
{
    if (!is_valid_channel_type(layout.channel_type)
        || !is_valid_sample_stream_layout(layout.sample_layout)) {
        throw std::invalid_argument(
            "sample region has an invalid channel layout");
    }
    if (!is_registered_continuous_value_type(geometry.value_type)
        || geometry.value_size_bytes == 0
        || geometry.element_alignment == 0
        || !std::has_single_bit(geometry.element_alignment)) {
        throw std::invalid_argument(
            "sample region has invalid continuous-value geometry");
    }
}

void validate(PersistedSampleRegion const& region)
{
    if (!region.output.valid()
        || persisted_output_kind(region.output) != PortKind::sample) {
        throw std::invalid_argument(
            "sample region requires a sample output identity");
    }
    validate_sample_representation(region.layout, region.geometry);
    if (region.windows) {
        if (!region.values.empty() || !region.continuous_values.empty()
            || !region.window_views || region.window_views->size() != region.windows->size())
            throw std::invalid_argument("windowed sample Region has inconsistent owners");
        IndexCoverage prepared;
        for (auto const& window : *region.windows) {
            validate(window);
            if (prepared.intersects(window.range))
                throw std::invalid_argument("sample content windows overlap within one logical Region");
            prepared.include(window.range);
            if (window.windows || !region.range.contains(window.range) || window.output != region.output
                || window.layout != region.layout || window.geometry != region.geometry)
                throw std::invalid_argument("sample content window lies outside its logical Region");
        }
        return;
    }
    auto const frames = checked_frame_count(region.range);
    auto const values = checked_product(frames, channel_count(region.layout));
    auto const expected_bytes = checked_product(
        values, region.geometry.value_size_bytes);
    auto const sample_geometry = *continuous_value_storage_geometry(
        continuous_value_type_id_v<Sample>, std::nullopt);
    auto const valid_sample_payload = region.geometry == sample_geometry
        && region.continuous_values.empty()
        && region.values.size() == values;
    auto const valid_continuous_payload = region.values.empty()
        && region.continuous_values.size() == expected_bytes
        && region.continuous_values.alignment()
            == region.geometry.element_alignment;
    if (!valid_sample_payload && !valid_continuous_payload) {
        throw std::invalid_argument(
            "sample region payload does not match its range and layout");
    }
}

void validate(PersistedEventChunk const& chunk)
{
    if (!chunk.output.valid()
        || persisted_output_kind(chunk.output) != PortKind::event) {
        throw std::invalid_argument(
            "event chunk requires an event output identity");
    }
    if (chunk.range.empty() || chunk.type >= EventTypeId::count) {
        throw std::invalid_argument("event chunk metadata is invalid");
    }
    if (chunk.prepared_coverage
        && !(*chunk.prepared_coverage - IndexCoverage{chunk.range}).empty())
        throw std::invalid_argument("prepared event coverage lies outside its logical Region");
    GlobalIndex::extent_type previous = 0;
    bool first = true;
    for (auto const& event : chunk.events) {
        auto const offset = chunk.range.offset_of(event.time);
        if (!offset || (chunk.prepared_coverage && !chunk.prepared_coverage->contains(event.time))
            || !event_matches_type(chunk.type, event.value)) {
            throw std::invalid_argument(
                "event chunk contains an event outside its typed range");
        }
        if (!first && *offset < previous) {
            throw std::invalid_argument(
                "event chunk events are not in range-relative order");
        }
        previous = *offset;
        first = false;
    }
}

template<typename Payload>
std::vector<Payload const*> writes_by_region(
    std::span<GlobalIndexRange const> logical_order,
    std::vector<PersistedRegionWrite<Payload>> const& writes)
{
    std::vector<Payload const*> result(logical_order.size());
    for (auto const& write : writes) {
        auto const index = write.region_index;
        if (index >= result.size() || result[index] || write.payload.range != logical_order[index])
            throw std::invalid_argument("persisted write does not identify one published Region");
        validate(write.payload);
        result[index] = &write.payload;
    }
    return result;
}

[[nodiscard]] PersistedSampleRegion map_buffered_slice(PersistedSampleRegion const&, GlobalIndexRange);
[[nodiscard]] PersistedSampleRegion page_backed_region(PersistedSampleRegion,
    std::shared_ptr<BufferedPageAllocator> const&);

[[nodiscard]] PersistedSampleRegion slice(
    PersistedSampleRegion const& source,
    GlobalIndexRange range)
{
    assert(source.range.contains(range) && !range.empty());
    if (source.buffered_backing) return map_buffered_slice(source, range);
    auto const source_frames = checked_frame_count(source.range);
    auto const frames = checked_frame_count(range);
    auto const frame_offset = static_cast<std::size_t>(
        source.range.begin.forward_distance_to(range.begin));
    auto const channels = channel_count(source.layout);
    PersistedSampleRegion result{
        .output = source.output,
        .range = range,
        .layout = source.layout,
        .geometry = source.geometry,
    };
    auto const value_count = checked_product(frames, channels);
    if (!source.values.empty()) {
        result.values.resize(value_count);
    } else {
        result.continuous_values = PersistedContinuousStorage{
            checked_product(value_count, source.geometry.value_size_bytes),
            source.geometry.element_alignment};
    }
    auto copy_values = [&](std::size_t source_value,
                           std::size_t target_value,
                           std::size_t count) {
        std::memcpy(
            result.storage_data()
                + target_value * source.geometry.value_size_bytes,
            source.storage_data()
                + source_value * source.geometry.value_size_bytes,
            count * source.geometry.value_size_bytes);
    };
    if (source.layout.sample_layout == SampleStreamLayout::planar) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            copy_values(
                channel * source_frames + frame_offset,
                channel * frames,
                frames);
        }
    } else {
        auto const value_offset = checked_product(frame_offset, channels);
        copy_values(value_offset, 0, value_count);
    }
    return result;
}

[[nodiscard]] PersistedEventChunk slice(
    PersistedEventChunk const& source,
    GlobalIndexRange range)
{
    assert(source.range.contains(range) && !range.empty());
    PersistedEventChunk result{
        .output = source.output,
        .range = range,
        .type = source.type,
    };
    result.events.reserve(source.events.size());
    std::ranges::copy_if(
        source.events,
        std::back_inserter(result.events),
        [&](TimedEvent const& event) { return range.contains(event.time); });
    return result;
}

template<typename Node>
[[nodiscard]] std::uint32_t node_height(
    std::shared_ptr<Node const> const& node) noexcept
{
    return node ? node->height : 0;
}

template<typename Record>
[[nodiscard]] std::shared_ptr<RangeNode<Record> const> make_range_node(
    std::shared_ptr<Record const> record,
    std::shared_ptr<RangeNode<Record> const> left = {},
    std::shared_ptr<RangeNode<Record> const> right = {})
{
    auto const height = 1 + std::max(node_height(left), node_height(right));
    return std::make_shared<RangeNode<Record> const>(RangeNode<Record>{
        .record = std::move(record),
        .left = std::move(left),
        .right = std::move(right),
        .height = height,
    });
}

template<typename Record>
[[nodiscard]] std::shared_ptr<RangeNode<Record> const> balance_range_node(
    std::shared_ptr<RangeNode<Record> const> node)
{
    auto const balance = static_cast<int>(node_height(node->left))
        - static_cast<int>(node_height(node->right));
    if (balance > 1) {
        auto left = node->left;
        if (node_height(left->left) < node_height(left->right)) {
            auto const pivot = left->right;
            left = make_range_node<Record>(
                left->record,
                left->left,
                pivot->left);
            left = make_range_node<Record>(
                pivot->record,
                std::move(left),
                pivot->right);
        }
        auto const new_right = make_range_node<Record>(
            node->record, left->right, node->right);
        return make_range_node<Record>(
            left->record, left->left, std::move(new_right));
    }
    if (balance < -1) {
        auto right = node->right;
        if (node_height(right->right) < node_height(right->left)) {
            auto const pivot = right->left;
            right = make_range_node<Record>(
                right->record,
                pivot->right,
                right->right);
            right = make_range_node<Record>(
                pivot->record,
                pivot->left,
                std::move(right));
        }
        auto const new_left = make_range_node<Record>(
            node->record, node->left, right->left);
        return make_range_node<Record>(
            right->record, std::move(new_left), right->right);
    }
    return node;
}

template<typename Record>
[[nodiscard]] GlobalIndex::storage_type record_key(Record const& record) noexcept
{
    return SignedGlobalIndexOrder::key(record.range.begin);
}

template<typename Record>
[[nodiscard]] std::shared_ptr<RangeNode<Record> const> put_record(
    std::shared_ptr<RangeNode<Record> const> const& root,
    std::shared_ptr<Record const> record)
{
    if (!root) return make_range_node<Record>(std::move(record));
    auto const key = record_key(*record);
    auto const root_key = record_key(*root->record);
    if (key < root_key) {
        return balance_range_node<Record>(make_range_node<Record>(
            root->record,
            put_record(root->left, std::move(record)),
            root->right));
    }
    if (root_key < key) {
        return balance_range_node<Record>(make_range_node<Record>(
            root->record,
            root->left,
            put_record(root->right, std::move(record))));
    }
    return make_range_node<Record>(
        std::move(record), root->left, root->right);
}

template<typename Record>
[[nodiscard]] std::shared_ptr<RangeNode<Record> const> erase_record(
    std::shared_ptr<RangeNode<Record> const> const& root,
    GlobalIndex::storage_type key)
{
    if (!root) return {};
    auto const root_key = record_key(*root->record);
    if (key < root_key) {
        return balance_range_node<Record>(make_range_node<Record>(
            root->record,
            erase_record(root->left, key),
            root->right));
    }
    if (root_key < key) {
        return balance_range_node<Record>(make_range_node<Record>(
            root->record,
            root->left,
            erase_record(root->right, key)));
    }
    if (!root->left) return root->right;
    if (!root->right) return root->left;
    auto successor = root->right;
    while (successor->left) successor = successor->left;
    return balance_range_node<Record>(make_range_node<Record>(
        successor->record,
        root->left,
        erase_record(root->right, record_key(*successor->record))));
}

template<typename Record>
[[nodiscard]] Record const* find_record(
    std::shared_ptr<RangeNode<Record> const> const& root,
    GlobalIndex index) noexcept
{
    auto const key = SignedGlobalIndexOrder::key(index);
    auto const* node = root.get();
    Record const* candidate = nullptr;
    while (node) {
        auto const node_key = record_key(*node->record);
        if (key < node_key) {
            node = node->left.get();
        } else {
            candidate = node->record.get();
            node = node->right.get();
        }
    }
    return candidate && candidate->range.contains(index) ? candidate : nullptr;
}

template<typename Record, typename Fn>
void for_each_intersecting(
    std::shared_ptr<RangeNode<Record> const> const& root,
    GlobalIndexRange query,
    Fn&& fn)
{
    if (!root) return;
    auto const stored = linear_range(root->record->range);
    auto const requested = linear_range(query);
    if (!less(stored.begin, requested.end)) {
        for_each_intersecting(root->left, query, std::forward<Fn>(fn));
        return;
    }
    if (!less(requested.begin, stored.end)) {
        for_each_intersecting(root->right, query, std::forward<Fn>(fn));
        return;
    }
    for_each_intersecting(root->left, query, fn);
    fn(root->record);
    for_each_intersecting(root->right, query, fn);
}

template<typename Record, typename Fn>
void for_each_record(
    std::shared_ptr<RangeNode<Record> const> const& root,
    Fn&& fn)
{
    if (!root) return;
    for_each_record(root->left, fn);
    fn(*root->record);
    for_each_record(root->right, fn);
}

[[nodiscard]] std::shared_ptr<void const> account_event_contents(
    std::shared_ptr<BufferedPageAllocator> const& allocator,
    std::shared_ptr<std::vector<PersistedEventChunk> const> const& logical,
    std::shared_ptr<RangeNode<PersistedEventChunk> const> const& chunks,
    std::shared_ptr<std::vector<TimedEvent> const> const& timeline)
{
    auto account = [&](PersistedEventChunk const& chunk) {
        auto const bytes = checked_product(chunk.events.capacity(), sizeof(TimedEvent));
        if (chunk.accounted_storage == chunk.events.data() && chunk.accounted_bytes == bytes) return;
        auto charge = allocator->retain_event_bytes(bytes);
        chunk.storage_accounting = std::move(charge);
        chunk.accounted_storage = chunk.events.data();
        chunk.accounted_bytes = bytes;
    };
    if (logical) for (auto const& chunk : *logical) account(chunk);
    for_each_record(chunks, account);
    return allocator->retain_event_bytes(timeline ? checked_product(timeline->capacity(), sizeof(TimedEvent)) : 0);
}

[[nodiscard]] std::shared_ptr<BufferedPageNode const> make_buffered_page_node(
    BufferedPageKey key,
    std::shared_ptr<BufferedPage const> page,
    std::shared_ptr<BufferedPageNode const> left = {},
    std::shared_ptr<BufferedPageNode const> right = {})
{
    auto const height = 1 + std::max(node_height(left), node_height(right));
    return std::make_shared<BufferedPageNode const>(BufferedPageNode{
        .key = key,
        .page = std::move(page),
        .left = std::move(left),
        .right = std::move(right),
        .height = height,
    });
}

[[nodiscard]] std::shared_ptr<BufferedPageNode const> balance_buffered_page_node(
    std::shared_ptr<BufferedPageNode const> node)
{
    auto const balance = static_cast<int>(node_height(node->left))
        - static_cast<int>(node_height(node->right));
    if (balance > 1) {
        auto left = node->left;
        if (node_height(left->left) < node_height(left->right)) {
            auto const pivot = left->right;
            left = make_buffered_page_node(
                left->key, left->page, left->left, pivot->left);
            left = make_buffered_page_node(
                pivot->key, pivot->page, std::move(left), pivot->right);
        }
        auto const new_right = make_buffered_page_node(
            node->key, node->page, left->right, node->right);
        return make_buffered_page_node(
            left->key, left->page, left->left, std::move(new_right));
    }
    if (balance < -1) {
        auto right = node->right;
        if (node_height(right->right) < node_height(right->left)) {
            auto const pivot = right->left;
            right = make_buffered_page_node(
                right->key, right->page, pivot->right, right->right);
            right = make_buffered_page_node(
                pivot->key, pivot->page, pivot->left, std::move(right));
        }
        auto const new_left = make_buffered_page_node(
            node->key, node->page, node->left, right->left);
        return make_buffered_page_node(
            right->key, right->page, std::move(new_left), right->right);
    }
    return node;
}

[[nodiscard]] std::shared_ptr<BufferedPageNode const> put_buffered_page(
    std::shared_ptr<BufferedPageNode const> const& root,
    BufferedPageKey key,
    std::shared_ptr<BufferedPage const> page)
{
    if (!root) {
        return make_buffered_page_node(key, std::move(page));
    }
    if (key < root->key) {
        return balance_buffered_page_node(make_buffered_page_node(
            root->key,
            root->page,
            put_buffered_page(root->left, key, std::move(page)),
            root->right));
    }
    if (root->key < key) {
        return balance_buffered_page_node(make_buffered_page_node(
            root->key,
            root->page,
            root->left,
            put_buffered_page(root->right, key, std::move(page))));
    }
    return make_buffered_page_node(
        key, std::move(page), root->left, root->right);
}

[[nodiscard]] BufferedPage const* find_buffered_page(
    std::shared_ptr<BufferedPageNode const> const& root,
    BufferedPageKey key) noexcept
{
    auto const* node = root.get();
    while (node) {
        if (key < node->key) {
            node = node->left.get();
        } else if (node->key < key) {
            node = node->right.get();
        } else {
            return node->page.get();
        }
    }
    return nullptr;
}

[[nodiscard]] std::shared_ptr<BufferedPageNode const> erase_buffered_page(
    std::shared_ptr<BufferedPageNode const> const& root, BufferedPageKey key)
{
    if (!root) return {};
    if (key < root->key) return balance_buffered_page_node(make_buffered_page_node(root->key, root->page,
        erase_buffered_page(root->left, key), root->right));
    if (root->key < key) return balance_buffered_page_node(make_buffered_page_node(root->key, root->page,
        root->left, erase_buffered_page(root->right, key)));
    if (!root->left) return root->right;
    if (!root->right) return root->left;
    auto next = root->right;
    while (next->left) next = next->left;
    return balance_buffered_page_node(make_buffered_page_node(next->key, next->page,
        root->left, erase_buffered_page(root->right, next->key)));
}

using BufferedByteAddress = unsigned __int128;

[[nodiscard]] BufferedPageKey buffered_page_key(
    BufferedByteAddress address,
    std::size_t page_size) noexcept
{
    auto const page = address / page_size;
    return {
        .high = static_cast<std::uint64_t>(page >> 64),
        .low = static_cast<std::uint64_t>(page),
    };
}

[[nodiscard]] BufferedPageKey add_buffered_pages(
    BufferedPageKey key,
    std::size_t count) noexcept
{
    auto const previous = key.low;
    key.low += static_cast<std::uint64_t>(count);
    if (key.low < previous) ++key.high;
    return key;
}

using BufferedPageNumber = unsigned __int128;

[[nodiscard]] BufferedPageNumber buffered_page_number(
    BufferedPageKey key) noexcept
{
    return (static_cast<BufferedPageNumber>(key.high) << 64) | key.low;
}

[[nodiscard]] std::size_t checked_page_count_sum(
    std::size_t left,
    std::size_t right)
{
    if (left > std::numeric_limits<std::size_t>::max() - right) {
        throw std::length_error("recording page run is too large");
    }
    return left + right;
}

[[nodiscard]] std::shared_ptr<BufferedPageRunNode const>
make_buffered_page_run_node(
    BufferedPageRun run,
    std::shared_ptr<BufferedPageRunNode const> left = {},
    std::shared_ptr<BufferedPageRunNode const> right = {})
{
    auto const height = 1 + std::max(node_height(left), node_height(right));
    return std::make_shared<BufferedPageRunNode const>(BufferedPageRunNode{
        .run = run,
        .left = std::move(left),
        .right = std::move(right),
        .height = height,
    });
}

[[nodiscard]] std::shared_ptr<BufferedPageRunNode const>
balance_buffered_page_run_node(
    std::shared_ptr<BufferedPageRunNode const> node)
{
    auto const balance = static_cast<int>(node_height(node->left))
        - static_cast<int>(node_height(node->right));
    if (balance > 1) {
        auto left = node->left;
        if (node_height(left->left) < node_height(left->right)) {
            auto const pivot = left->right;
            left = make_buffered_page_run_node(
                left->run, left->left, pivot->left);
            left = make_buffered_page_run_node(
                pivot->run, std::move(left), pivot->right);
        }
        auto const new_right = make_buffered_page_run_node(
            node->run, left->right, node->right);
        return make_buffered_page_run_node(
            left->run, left->left, std::move(new_right));
    }
    if (balance < -1) {
        auto right = node->right;
        if (node_height(right->right) < node_height(right->left)) {
            auto const pivot = right->left;
            right = make_buffered_page_run_node(
                right->run, pivot->right, right->right);
            right = make_buffered_page_run_node(
                pivot->run, pivot->left, std::move(right));
        }
        auto const new_left = make_buffered_page_run_node(
            node->run, node->left, right->left);
        return make_buffered_page_run_node(
            right->run, std::move(new_left), right->right);
    }
    return node;
}

[[nodiscard]] std::shared_ptr<BufferedPageRunNode const>
put_buffered_page_run(
    std::shared_ptr<BufferedPageRunNode const> const& root,
    BufferedPageRun run)
{
    if (!root) return make_buffered_page_run_node(run);
    if (run.first < root->run.first) {
        return balance_buffered_page_run_node(make_buffered_page_run_node(
            root->run,
            put_buffered_page_run(root->left, run),
            root->right));
    }
    if (root->run.first < run.first) {
        return balance_buffered_page_run_node(make_buffered_page_run_node(
            root->run,
            root->left,
            put_buffered_page_run(root->right, run)));
    }
    return make_buffered_page_run_node(run, root->left, root->right);
}

[[nodiscard]] std::shared_ptr<BufferedPageRunNode const>
erase_buffered_page_run(
    std::shared_ptr<BufferedPageRunNode const> const& root,
    BufferedPageKey first)
{
    if (!root) return {};
    if (first < root->run.first) {
        return balance_buffered_page_run_node(make_buffered_page_run_node(
            root->run,
            erase_buffered_page_run(root->left, first),
            root->right));
    }
    if (root->run.first < first) {
        return balance_buffered_page_run_node(make_buffered_page_run_node(
            root->run,
            root->left,
            erase_buffered_page_run(root->right, first)));
    }
    if (!root->left) return root->right;
    if (!root->right) return root->left;
    auto successor = root->right;
    while (successor->left) successor = successor->left;
    return balance_buffered_page_run_node(make_buffered_page_run_node(
        successor->run,
        root->left,
        erase_buffered_page_run(root->right, successor->run.first)));
}

[[nodiscard]] BufferedPageRun const* find_buffered_page_run(
    std::shared_ptr<BufferedPageRunNode const> const& root,
    BufferedPageKey page) noexcept
{
    auto const page_number = buffered_page_number(page);
    auto const* node = root.get();
    BufferedPageRun const* candidate = nullptr;
    while (node) {
        if (page < node->run.first) {
            node = node->left.get();
        } else {
            candidate = std::addressof(node->run);
            node = node->right.get();
        }
    }
    if (!candidate) return nullptr;
    auto const offset = page_number
        - buffered_page_number(candidate->first);
    return offset < candidate->page_count ? candidate : nullptr;
}

[[nodiscard]] BufferedPageRun const* find_buffered_page_run_before(
    std::shared_ptr<BufferedPageRunNode const> const& root,
    BufferedPageKey first) noexcept
{
    auto const* node = root.get();
    BufferedPageRun const* result = nullptr;
    while (node) {
        if (node->run.first < first) {
            result = std::addressof(node->run);
            node = node->right.get();
        } else {
            node = node->left.get();
        }
    }
    return result;
}

[[nodiscard]] BufferedPageRun const* find_buffered_page_run_after(
    std::shared_ptr<BufferedPageRunNode const> const& root,
    BufferedPageKey first) noexcept
{
    auto const* node = root.get();
    BufferedPageRun const* result = nullptr;
    while (node) {
        if (first < node->run.first) {
            result = std::addressof(node->run);
            node = node->left.get();
        } else {
            node = node->right.get();
        }
    }
    return result;
}

[[nodiscard]] bool buffered_page_runs_are_contiguous(
    BufferedPageRun const& left,
    BufferedPageRun const& right,
    std::size_t page_size) noexcept
{
    return add_buffered_pages(left.first, left.page_count) == right.first
        && left.file_offset + left.page_count * page_size
            == right.file_offset;
}

[[nodiscard]] std::shared_ptr<BufferedPageRunNode const>
put_coalesced_buffered_page_run(
    std::shared_ptr<BufferedPageRunNode const> root,
    BufferedPageRun run,
    std::size_t page_size)
{
    if (auto const* previous = find_buffered_page_run_before(
            root, run.first);
        previous && buffered_page_runs_are_contiguous(
            *previous, run, page_size)) {
        auto const first = previous->first;
        auto const file_offset = previous->file_offset;
        auto const page_count = previous->page_count;
        root = erase_buffered_page_run(root, first);
        run.first = first;
        run.file_offset = file_offset;
        run.page_count = checked_page_count_sum(
            page_count, run.page_count);
    }
    if (auto const* next = find_buffered_page_run_after(root, run.first);
        next && buffered_page_runs_are_contiguous(run, *next, page_size)) {
        auto const first = next->first;
        auto const page_count = next->page_count;
        root = erase_buffered_page_run(root, first);
        run.page_count = checked_page_count_sum(
            run.page_count, page_count);
    }
    return put_buffered_page_run(root, run);
}

[[nodiscard]] std::shared_ptr<BufferedPageRunNode const>
replace_buffered_page_run(
    std::shared_ptr<BufferedPageRunNode const> root,
    BufferedPageKey page,
    std::optional<std::uint64_t> file_offset,
    std::size_t page_size)
{
    if (auto const* previous = find_buffered_page_run(root, page)) {
        auto const old = *previous;
        root = erase_buffered_page_run(root, old.first);
        auto const prefix_number = buffered_page_number(page)
            - buffered_page_number(old.first);
        auto const prefix = static_cast<std::size_t>(prefix_number);
        assert(prefix < old.page_count);
        if (prefix != 0) {
            root = put_buffered_page_run(root, BufferedPageRun{
                .first = old.first,
                .page_count = prefix,
                .file_offset = old.file_offset,
            });
        }
        auto const suffix = old.page_count - prefix - 1;
        if (suffix != 0) {
            root = put_buffered_page_run(root, BufferedPageRun{
                .first = add_buffered_pages(page, 1),
                .page_count = suffix,
                .file_offset = old.file_offset
                    + (prefix + 1) * page_size,
            });
        }
    }
    if (!file_offset) return root;
    return put_coalesced_buffered_page_run(
        std::move(root),
        BufferedPageRun{
            .first = page,
            .page_count = 1,
            .file_offset = *file_offset,
        },
        page_size);
}

[[nodiscard]] BufferedByteAddress buffered_byte_address(
    GlobalIndex index,
    std::size_t bytes_per_frame) noexcept
{
    return static_cast<BufferedByteAddress>(
        SignedGlobalIndexOrder::key(index)) * bytes_per_frame;
}

[[nodiscard]] std::size_t buffered_bytes_per_frame(
    ChannelLayout layout,
    ContinuousValueStorageGeometry geometry)
{
    return checked_product(
        channel_count(layout), geometry.value_size_bytes);
}

void prune_erased_pages(std::shared_ptr<BufferedPageNode const>& pages,
    std::shared_ptr<BufferedPageRunNode const>& runs, IndexCoverage const& retained,
    GlobalIndexRange erased, std::size_t bytes_per_frame, std::size_t page_size)
{
    std::vector<BufferedPageKey> removed;
    erased.for_each_signed_order_segment([&](GlobalIndexRange segment) {
        auto const begin = buffered_byte_address(segment.begin, bytes_per_frame);
        auto const end = begin + static_cast<BufferedByteAddress>(segment.extent) * bytes_per_frame;
        auto const first = buffered_page_key(begin, page_size), last = buffered_page_key(end - 1, page_size);
        auto visit = [&](auto&& self, std::shared_ptr<BufferedPageNode const> const& node) -> void {
            if (!node) return;
            if (first < node->key) self(self, node->left);
            if (first <= node->key && node->key <= last) {
                bool valid = false;
                for (auto const range : retained) range.for_each_signed_order_segment([&](GlobalIndexRange part) {
                    auto const a = buffered_byte_address(part.begin, bytes_per_frame);
                    auto const b = a + static_cast<BufferedByteAddress>(part.extent) * bytes_per_frame;
                    if (buffered_page_key(a, page_size) <= node->key && node->key <= buffered_page_key(b - 1, page_size)) valid = true;
                });
                if (!valid) removed.push_back(node->key);
            }
            if (node->key < last) self(self, node->right);
        };
        visit(visit, pages);
    });
    for (auto const key : removed) {
        pages = erase_buffered_page(pages, key);
        runs = replace_buffered_page_run(runs, key, std::nullopt, page_size);
    }
}

void write_recording_region(
    std::shared_ptr<BufferedPageNode const>& root,
    std::shared_ptr<BufferedPageRunNode const>& runs,
    BufferedPageAllocator& allocator,
    PersistedSampleRegion const& record)
{
    assert(record.layout.sample_layout == SampleStreamLayout::interleaved);
    auto const bytes_per_frame = buffered_bytes_per_frame(
        record.layout, record.geometry);
    auto const page_size = allocator.page_size();
    record.range.for_each_signed_order_segment([&](GlobalIndexRange segment) {
        auto const source_frame = static_cast<std::size_t>(
            record.range.begin.forward_distance_to(segment.begin));
        auto const segment_frames = checked_frame_count(segment);
        auto const source_offset = checked_product(
            source_frame, bytes_per_frame);
        auto remaining = checked_product(segment_frames, bytes_per_frame);
        auto const* source = record.storage_data() + source_offset;
        auto address = buffered_byte_address(
            segment.begin, bytes_per_frame);
        while (remaining != 0) {
            auto const key = buffered_page_key(address, page_size);
            auto const in_page = static_cast<std::size_t>(address % page_size);
            auto const count = std::min(remaining, page_size - in_page);
            std::vector<std::byte> contents(page_size);
            bool unchanged = false;
            if (auto const* previous = find_buffered_page(root, key)) {
                allocator.read(*previous, contents);
                unchanged = std::memcmp(contents.data() + in_page, source, count) == 0;
            }
            if (!unchanged) {
                std::memcpy(contents.data() + in_page, source, count);
                auto page = allocator.allocate(contents);
                runs = replace_buffered_page_run(std::move(runs), key, page->file_offset, page_size);
                root = put_buffered_page(root, key, std::move(page));
            }
            source += count;
            remaining -= count;
            address += count;
        }
    });
}

class BufferedRegionMapping {
    void* mapping_ = nullptr;
    std::size_t mapping_size_ = 0;

public:
    std::shared_ptr<BufferedPageAllocator> allocator{};
    std::shared_ptr<void> accounting{};
    std::vector<std::shared_ptr<void const>> page_pins{};

    BufferedRegionMapping(
        void* mapping,
        std::size_t mapping_size,
        std::shared_ptr<BufferedPageAllocator> allocator_value,
        std::shared_ptr<void> charge,
        std::vector<std::shared_ptr<void const>> pins) noexcept
        : mapping_(mapping)
        , mapping_size_(mapping_size)
        , allocator(std::move(allocator_value))
        , accounting(std::move(charge))
        , page_pins(std::move(pins))
    {}

    ~BufferedRegionMapping()
    {
#if defined(__linux__)
        if (mapping_) {
            PlaybackPreparationMetrics::Interval timing{PlaybackPreparationPhase::munmap_call, mapping_size_};
            persisted_data_store_detail::ProcessVmaBudget::account_for_growth(1);
            ::munmap(mapping_, mapping_size_);
        }
#endif
    }

    BufferedRegionMapping(BufferedRegionMapping const&) = delete;
    BufferedRegionMapping& operator=(BufferedRegionMapping const&) = delete;

    [[nodiscard]] std::byte const* data() const noexcept { return static_cast<std::byte const*>(mapping_); }
};

#if defined(__linux__)
// Experiment with one stable virtual address for each offset in the page file.
// A Region still pins its own pages. Only a single physically contiguous run
// can use a single pointer into this mapping.
[[nodiscard]] std::shared_ptr<BufferedRegionMapping const> playback_benchmark_arena(
    std::shared_ptr<BufferedPageAllocator> const& allocator, std::uint64_t first_byte, std::size_t bytes)
{
    static thread_local std::weak_ptr<BufferedRegionMapping const> cached;
    auto const page = allocator->page_size();
    auto const& limits = allocator->limits();
    auto const size = std::min(limits.max_backing_bytes, limits.max_contiguous_mapping_bytes) / page * page;
    if (first_byte > size || bytes > size - first_byte) return {};
    if (auto arena = cached.lock(); arena && arena->allocator == allocator) return arena;

    allocator->reserve_mapping(size, 1);
    auto charge = std::shared_ptr<void>(nullptr, [allocator, size](void*) noexcept {
        allocator->release_mapping(size, 1);
    });
    auto* mapping = measure_playback_preparation(PlaybackPreparationPhase::mmap_backing, [&] {
        auto* address = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, allocator->file(), 0);
        if (address == MAP_FAILED)
            throw std::system_error(errno, std::generic_category(), "cannot map the playback benchmark arena");
        return address;
    }, size);
    auto unmap = [size](void* address) noexcept {
        persisted_data_store_detail::ProcessVmaBudget::account_for_growth(1);
        ::munmap(address, size);
    };
    std::unique_ptr<void, decltype(unmap)> guard{mapping, unmap};
    auto arena = std::make_shared<BufferedRegionMapping const>(mapping, size, allocator, std::move(charge),
        std::vector<std::shared_ptr<void const>>{});
    (void)guard.release();
    cached = arena;
    return arena;
}
#endif

[[nodiscard]] std::shared_ptr<BufferedPageNode const> retain_page_range(
    std::shared_ptr<BufferedPageNode const> const& root, BufferedPageKey first, BufferedPageKey last,
    bool lower = true, bool upper = true)
{
    if (!root) return {};
    if (!lower && !upper) return root;
    if (lower && root->key < first) return retain_page_range(root->right, first, last, true, upper);
    if (upper && last < root->key) return retain_page_range(root->left, first, last, lower, true);
    auto left = retain_page_range(root->left, first, last, lower, false);
    auto right = retain_page_range(root->right, first, last, false, upper);
    if (left == root->left && right == root->right) return root;
    // This immutable view index is never used as an insertion tree.
    return make_buffered_page_node(root->key, root->page, std::move(left), std::move(right));
}

[[nodiscard]] bool same_buffered_runs(BufferedPageRunNode const* old_runs,
    std::shared_ptr<BufferedPageRunNode const> const& next_runs, BufferedPageKey first,
    std::size_t count, std::size_t page_size)
{
    // Both indexes are immutable. Compare physical runs, never walk each page.
    auto old = [&](BufferedPageKey key) -> BufferedPageRun const* {
        BufferedPageRun const* found = nullptr;
        auto* node = old_runs;
        while (node) {
            if (key < node->run.first) node = node->left.get();
            else { found = &node->run; node = node->right.get(); }
        }
        return found && buffered_page_number(key) - buffered_page_number(found->first) < found->page_count ? found : nullptr;
    };
    while (count) {
        auto const* before = old(first);
        auto const* after = find_buffered_page_run(next_runs, first);
        if (!before || !after) return false;
        auto const a = static_cast<std::size_t>(buffered_page_number(first) - buffered_page_number(before->first));
        auto const b = static_cast<std::size_t>(buffered_page_number(first) - buffered_page_number(after->first));
        if (before->file_offset + a * page_size != after->file_offset + b * page_size) return false;
        auto const common = std::min({count, before->page_count - a, after->page_count - b});
        first = add_buffered_pages(first, common);
        count -= common;
    }
    return true;
}

// Pin fully enclosed subtrees in one operation and visit only the two boundary
// paths. A small mapped window must not retain every page of its old output.
void pin_buffered_pages(std::shared_ptr<BufferedPageNode const> const& root,
    BufferedPageKey first, BufferedPageKey last,
    std::vector<std::shared_ptr<void const>>& pins, bool lower = true, bool upper = true)
{
    if (!root) return;
    if (!lower && !upper) { pins.push_back(root); return; }
    if (lower && root->key < first) {
        pin_buffered_pages(root->right, first, last, pins, true, upper);
        return;
    }
    if (upper && last < root->key) {
        pin_buffered_pages(root->left, first, last, pins, lower, true);
        return;
    }
    pins.push_back(root->page);
    pin_buffered_pages(root->left, first, last, pins, lower, false);
    pin_buffered_pages(root->right, first, last, pins, false, upper);
}

[[nodiscard]] PersistedSampleRegion map_buffered_region(
    PersistedOutputHandle output,
    GlobalIndexRange range,
    ChannelLayout layout,
    ContinuousValueStorageGeometry geometry,
    std::shared_ptr<BufferedPageAllocator> const& allocator,
    std::shared_ptr<BufferedPageRunNode const> const& runs,
    std::shared_ptr<BufferedPageNode const> const& pages,
    bool retain_backing = false,
    std::optional<BufferedByteAddress> byte_begin = {},
    std::vector<std::shared_ptr<void const>> extra_pins = {})
{
    PlaybackPreparationMetrics::Interval timing{PlaybackPreparationPhase::map_sample_view};
    auto const bytes_per_frame = buffered_bytes_per_frame(layout, geometry);
    auto const bytes = checked_product(
        checked_frame_count(range), bytes_per_frame);
    auto const page_size = allocator->page_size();
    auto const address = byte_begin.value_or(buffered_byte_address(range.begin, bytes_per_frame));
    auto const in_page = static_cast<std::size_t>(address % page_size);
    if (bytes > std::numeric_limits<std::size_t>::max() - in_page) {
        throw std::length_error("buffered Region mapping is too large");
    }
    auto const occupied = in_page + bytes;
    auto const page_count = occupied / page_size
        + static_cast<std::size_t>(occupied % page_size != 0);
    auto const mapping_size = checked_product(page_count, page_size);
    timing.set_work(mapping_size);
    if (mapping_size > allocator->limits().max_contiguous_mapping_bytes)
        throw std::length_error("buffered contiguous view requires " + std::to_string(mapping_size)
            + " bytes; limit " + std::to_string(allocator->limits().max_contiguous_mapping_bytes));
#if defined(__linux__)
    auto const first_key = buffered_page_key(address, page_size);
    std::vector<BufferedPageRun> selected_runs;
    for (std::size_t mapped_pages = 0; mapped_pages < page_count;) {
        auto const key = add_buffered_pages(first_key, mapped_pages);
        auto const* run = find_buffered_page_run(runs, key);
        if (!run) throw std::logic_error("buffered coverage references an unallocated page");
        auto const offset = static_cast<std::size_t>(buffered_page_number(key) - buffered_page_number(run->first));
        auto const count = std::min(page_count - mapped_pages, run->page_count - offset);
        selected_runs.push_back({.first = key, .page_count = count,
            .file_offset = run->file_offset + offset * page_size});
        if (selected_runs.size() > allocator->limits().max_mapping_runs)
            throw std::length_error("buffered view mapping-run limit exceeded");
        mapped_pages += count;
    }
    auto backing = retain_backing ? std::make_shared<BufferedSampleBacking>(BufferedSampleBacking{
        .allocator = allocator, .pages = retain_page_range(pages, first_key, add_buffered_pages(first_key, page_count - 1)),
        .runs = runs, .origin = range.begin, .byte_begin = address,
        .plane_stride_bytes = layout.sample_layout == SampleStreamLayout::planar
            ? static_cast<BufferedByteAddress>(range.extent) * geometry.value_size_bytes : 0}) : nullptr;
    auto pins = std::move(extra_pins);
    pin_buffered_pages(pages, first_key, add_buffered_pages(first_key, page_count - 1), pins);
    auto const use_benchmark_arena = PlaybackPreparationMetrics::active_sample_preparation()
        == PlaybackPreparationMetrics::SamplePreparation::arena;
    if (use_benchmark_arena && selected_runs.size() == 1) {
        auto const offset = selected_runs.front().file_offset;
        if (auto arena = playback_benchmark_arena(allocator, offset, mapping_size)) {
            PlaybackPreparationMetrics::Interval timing{PlaybackPreparationPhase::stable_sample_window, mapping_size};
            auto const* storage = arena->data() + offset + in_page;
            pins.push_back(std::move(arena));
            auto owner = std::make_shared<BufferedRegionMapping const>(nullptr, 0, allocator, nullptr, std::move(pins));
            return {.output = output, .range = range, .layout = layout, .geometry = geometry,
                .continuous_values = PersistedContinuousStorage::mapped(
                    std::move(owner), storage, bytes, geometry.element_alignment),
                .buffered_backing = std::move(backing)};
        }
    }
    allocator->reserve_mapping(mapping_size, selected_runs.size());
    // Shared ownership also makes exceptional paths release the mapping charge.
    auto charge = std::shared_ptr<void>(nullptr, [allocator, mapping_size, count = selected_runs.size()](void*) noexcept {
        allocator->release_mapping(mapping_size, count);
    });
    auto* mapping = measure_playback_preparation(PlaybackPreparationPhase::mmap_reserve, [&] {
        auto* const reservation = ::mmap(nullptr, mapping_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (reservation == MAP_FAILED)
            throw std::system_error(errno, std::generic_category(), "cannot reserve a buffered Region mapping");
        return reservation;
    }, mapping_size);
    auto unmap = [mapping_size](void* address) noexcept {
        persisted_data_store_detail::ProcessVmaBudget::account_for_growth(1);
        ::munmap(address, mapping_size);
    };
    std::unique_ptr<void, decltype(unmap)> mapping_guard{mapping, unmap};
    std::size_t mapped_pages = 0;
    for (auto const& run : selected_runs) {
        auto const run_pages = run.page_count;
        auto const run_size = checked_product(run_pages, page_size);
        auto* const target = static_cast<std::byte*>(mapping)
            + mapped_pages * page_size;
        PlaybackPreparationMetrics::Interval map_run{PlaybackPreparationPhase::mmap_backing, run_size};
        if (::mmap(
                target,
                run_size,
                PROT_READ,
                MAP_SHARED | MAP_FIXED,
                allocator->file(),
                static_cast<off_t>(run.file_offset)) == MAP_FAILED) {
            throw std::system_error(
                errno, std::generic_category(), "cannot map buffered Region pages");
        }
        mapped_pages += run_pages;
    }
    auto owner = std::make_shared<BufferedRegionMapping const>(
        mapping, mapping_size, allocator, std::move(charge), std::move(pins));
    (void)mapping_guard.release();
    return PersistedSampleRegion{
        .output = output,
        .range = range,
        .layout = layout,
        .geometry = geometry,
        .continuous_values = PersistedContinuousStorage::mapped(
            std::move(owner),
            static_cast<std::byte const*>(mapping) + in_page,
            bytes,
            geometry.element_alignment),
        .buffered_backing = std::move(backing),
    };
#else
    (void)output;
    (void)runs;
    (void)pages;
    (void)retain_backing;
    (void)extra_pins;
    throw std::runtime_error(
        "buffered sample storage requires Linux virtual memory");
#endif
}

[[nodiscard]] std::shared_ptr<std::vector<PersistedSampleRegion> const>
make_recording_regions(
    PersistedOutputHandle output,
    IndexCoverage const& coverage,
    ChannelLayout layout,
    ContinuousValueStorageGeometry geometry,
    std::shared_ptr<BufferedPageAllocator> const& allocator,
    std::shared_ptr<BufferedPageRunNode const> const& runs,
    std::shared_ptr<BufferedPageNode const> const& pages,
    std::shared_ptr<std::vector<PersistedSampleRegion> const> const& previous = {})
{
    auto regions = std::make_shared<std::vector<PersistedSampleRegion>>();
    regions->reserve(coverage.size());
    for (auto const range : coverage) {
        PersistedSampleRegion logical{.output = output, .range = range, .layout = layout, .geometry = geometry};
        auto const frame_bytes = buffered_bytes_per_frame(layout, geometry);
        auto const page_size = allocator->page_size();
        auto const preferred = std::min<std::size_t>(allocator->limits().max_contiguous_mapping_bytes, 1024 * 1024)
            / page_size * page_size;
        std::vector<PersistedSampleRegion> windows;
        auto begin = range.begin;
        auto remaining = range.extent;
        while (remaining) {
            auto const address = buffered_byte_address(begin, frame_bytes);
            auto const offset = static_cast<std::size_t>(address % page_size);
            auto const frames = preferred > offset ? (preferred - offset) / frame_bytes : 0;
            if (!frames) throw std::length_error("buffered view limit cannot fit one aligned frame");
            GlobalIndexRange part{begin, std::min<std::uint64_t>(remaining, frames)};
            PersistedSampleRegion const* reusable = nullptr;
            if (previous) for (auto const& old_region : *previous) {
                auto const* window = old_region.prepared_window(part.begin);
                if (!window || window->range != part || !window->buffered_backing) continue;
                auto const bytes = checked_product(static_cast<std::size_t>(part.extent), frame_bytes);
                auto const count = (offset + bytes + page_size - 1) / page_size;
                if (same_buffered_runs(window->buffered_backing->runs.get(), runs,
                    buffered_page_key(address, page_size), count, page_size)) reusable = window;
                break;
            }
            windows.push_back(reusable ? *reusable
                : map_buffered_region(output, part, layout, geometry, allocator, runs, pages, true));
            begin = begin.advanced_by(part.extent);
            remaining -= part.extent;
        }
        if (windows.size() == 1) logical = std::move(windows.front());
        else {
            logical.set_prepared_windows(std::move(windows));
            logical.buffered_backing = std::make_shared<BufferedSampleBacking>(BufferedSampleBacking{
                .allocator = allocator, .pages = pages, .runs = runs, .origin = range.begin,
                .byte_begin = buffered_byte_address(range.begin, frame_bytes)});
        }
        regions->push_back(std::move(logical));
    }
    return std::shared_ptr<std::vector<PersistedSampleRegion> const>{
        std::move(regions)};
}

// Native byte intervals in a logical Region's stable page directory.
using BufferedByteSpan = std::pair<BufferedByteAddress, std::size_t>;
std::vector<BufferedByteSpan> buffered_byte_spans(BufferedSampleBacking const& backing,
    ChannelLayout layout, ContinuousValueStorageGeometry geometry, GlobalIndexRange range)
{
    auto const offset = static_cast<BufferedByteAddress>(backing.origin.forward_distance_to(range.begin));
    if (layout.sample_layout != SampleStreamLayout::planar || channel_count(layout) == 1)
        return {{backing.byte_begin + offset * buffered_bytes_per_frame(layout, geometry),
            checked_product(checked_frame_count(range), buffered_bytes_per_frame(layout, geometry))}};
    std::vector<BufferedByteSpan> result;
    for (std::size_t channel = 0; channel < channel_count(layout); ++channel)
        result.emplace_back(backing.byte_begin + channel * backing.plane_stride_bytes + offset * geometry.value_size_bytes,
            checked_product(checked_frame_count(range), geometry.value_size_bytes));
    return result;
}

// These trimmed read indexes never enter the mutable AVL directory.
std::shared_ptr<BufferedPageNode const> remove_first_view_page(std::shared_ptr<BufferedPageNode const> const& root)
{
    if (!root->left) return root->right;
    return make_buffered_page_node(root->key, root->page, remove_first_view_page(root->left), root->right);
}
std::shared_ptr<BufferedPageNode const> join_page_views(std::shared_ptr<BufferedPageNode const> left,
    std::shared_ptr<BufferedPageNode const> right)
{
    if (!left) return right;
    if (!right) return left;
    auto first = right;
    while (first->left) first = first->left;
    return make_buffered_page_node(first->key, first->page, std::move(left), remove_first_view_page(right));
}
std::shared_ptr<BufferedSampleBacking const> select_buffered_backing(BufferedSampleBacking const& backing,
    std::span<BufferedByteSpan const> spans)
{
    auto selected = std::make_shared<BufferedSampleBacking>(backing);
    std::vector<std::pair<BufferedPageKey, BufferedPageKey>> intervals;
    auto const page_size = backing.allocator->page_size();
    for (auto [begin, bytes] : spans) {
        auto const first = buffered_page_key(begin, page_size), last = buffered_page_key(begin + bytes - 1, page_size);
        if (!intervals.empty() && first <= add_buffered_pages(intervals.back().second, 1))
            intervals.back().second = std::max(intervals.back().second, last);
        else intervals.emplace_back(first, last);
    }
    selected->pages = {};
    for (auto [first, last] : intervals)
        selected->pages = join_page_views(selected->pages, retain_page_range(backing.pages, first, last));
    return selected;
}

void read_buffered_bytes(BufferedSampleBacking const& backing, BufferedByteAddress address,
    std::span<std::byte> destination, std::vector<std::byte>& scratch)
{
    auto const page_size = backing.allocator->page_size();
    while (!destination.empty()) {
        auto const* page = find_buffered_page(backing.pages, buffered_page_key(address, page_size));
        if (!page) throw std::logic_error("buffered valid contents have no backing page");
        backing.allocator->read(*page, scratch);
        auto const offset = static_cast<std::size_t>(address % page_size);
        auto const count = std::min(destination.size(), page_size - offset);
        std::memcpy(destination.data(), scratch.data() + offset, count);
        destination = destination.subspan(count);
        address += count;
    }
}

[[nodiscard]] PersistedSampleRegion map_buffered_slice(PersistedSampleRegion const& source, GlobalIndexRange range)
{
    auto const& backing = *source.buffered_backing;
    auto const spans = buffered_byte_spans(backing, source.layout, source.geometry, range);
    auto selected = select_buffered_backing(backing, spans);
    if (spans.size() == 1) {
        auto result = map_buffered_region(source.output, range, source.layout, source.geometry,
            backing.allocator, backing.runs, backing.pages, false, spans.front().first);
        result.buffered_backing = std::move(selected);
        return result;
    }
    auto const bytes = checked_product(checked_frame_count(range), buffered_bytes_per_frame(source.layout, source.geometry));
    auto const page_size = backing.allocator->page_size();
    auto const first_offset = static_cast<std::size_t>(spans.front().first % page_size);
    auto const limit = backing.allocator->limits().max_contiguous_mapping_bytes;
    if (bytes > limit) throw std::length_error("buffered planar view exceeds the contiguous view limit");
    bool direct = first_offset <= limit - bytes
        && (first_offset + bytes) / page_size + ((first_offset + bytes) % page_size != 0) <= limit / page_size;
    std::shared_ptr<BufferedPageRunNode const> view_runs;
    auto destination = first_offset;
    for (auto [address, count_bytes] : spans) {
        if (!direct) break;
        if (destination % page_size != address % page_size) { direct = false; break; }
        auto const first = buffered_page_key(address, page_size);
        auto const target = buffered_page_key(destination, page_size);
        auto const occupied = static_cast<std::size_t>(address % page_size) + count_bytes;
        auto const count = occupied / page_size + (occupied % page_size != 0);
        for (std::size_t done = 0; done < count;) {
            auto const key = add_buffered_pages(first, done);
            auto const* run = find_buffered_page_run(backing.runs, key);
            if (!run) throw std::logic_error("buffered planar view has no source run");
            auto const offset = static_cast<std::size_t>(buffered_page_number(key) - buffered_page_number(run->first));
            auto const pages = std::min(count - done, run->page_count - offset);
            auto const file_offset = run->file_offset + offset * page_size;
            auto const destination_key = add_buffered_pages(target, done);
            if (auto const* existing = find_buffered_page_run(view_runs, destination_key)) {
                auto const existing_offset = static_cast<std::size_t>(buffered_page_number(destination_key)
                    - buffered_page_number(existing->first));
                if (existing->file_offset + existing_offset * page_size != file_offset) { direct = false; break; }
                done += std::min(pages, existing->page_count - existing_offset);
            } else {
                view_runs = put_coalesced_buffered_page_run(view_runs,
                    {.first = destination_key, .page_count = pages, .file_offset = file_offset}, page_size);
                done += pages;
            }
        }
        destination += count_bytes;
    }
    if (direct) {
        auto result = map_buffered_region(source.output, range, source.layout, source.geometry,
            backing.allocator, view_runs, {}, false, BufferedByteAddress{first_offset}, {selected});
        result.buffered_backing = std::move(selected);
        return result;
    }
    // A native planar view may pack two partial planes into one page although
    // their source pages differ. Only that bounded prepared view is repacked;
    // canonical ownership remains the Region's shared page directory.
    PersistedSampleRegion packed{.output = source.output, .range = range, .layout = source.layout,
        .geometry = source.geometry, .continuous_values = PersistedContinuousStorage{bytes, source.geometry.element_alignment}};
    std::vector<std::byte> scratch(page_size);
    std::size_t cursor = 0;
    for (auto [address, count] : spans) {
        read_buffered_bytes(backing, address, {packed.storage_data() + cursor, count}, scratch);
        cursor += count;
    }
    auto result = page_backed_region(std::move(packed), backing.allocator);
    result.buffered_backing = std::move(selected);
    return result;
}

[[nodiscard]] PersistedSampleRegion page_backed_region(PersistedSampleRegion window,
    std::shared_ptr<BufferedPageAllocator> const& allocator)
{
    if (window.buffered_backing) return window;
    auto const page_size = allocator->page_size();
    BufferedByteAddress address = 0;
    auto remaining = window.storage_size_bytes();
    auto const* data = window.storage_data();
    auto const offset = static_cast<std::size_t>(address % page_size);
    auto const limit = allocator->limits().max_contiguous_mapping_bytes;
    if (offset > limit || remaining > limit - offset
        || (offset + remaining) / page_size + ((offset + remaining) % page_size != 0) > limit / page_size)
        throw std::length_error("buffered payload exceeds the contiguous view limit");
    std::shared_ptr<BufferedPageNode const> pages;
    std::shared_ptr<BufferedPageRunNode const> runs;
    // Private backing only for a bounded native-layout repack. Canonical Tock
    // contents are owned by replace_tock_region()'s independent directory.
    std::vector<std::byte> contents(page_size);
    while (remaining) {
        auto const key = buffered_page_key(address, page_size);
        auto const in_page = static_cast<std::size_t>(address % page_size);
        auto const count = std::min(remaining, page_size - in_page);
        std::fill(contents.begin(), contents.end(), std::byte{});
        std::memcpy(contents.data() + in_page, data, count);
        auto page = allocator->allocate(contents);
        runs = put_coalesced_buffered_page_run(runs,
            {.first = key, .page_count = 1, .file_offset = page->file_offset}, page_size);
        pages = put_buffered_page(pages, key, std::move(page));
        data += count;
        address += count;
        remaining -= count;
    }
    return map_buffered_region(window.output, window.range, window.layout, window.geometry, allocator, runs, pages, true, BufferedByteAddress{0});
}

using BufferedPageInterval = std::pair<BufferedPageKey, BufferedPageKey>;
std::vector<BufferedPageInterval> valid_page_intervals(BufferedSampleBacking const& backing,
    ChannelLayout layout, ContinuousValueStorageGeometry geometry, IndexCoverage const& valid)
{
    std::vector<BufferedPageInterval> intervals;
    auto const page_size = backing.allocator->page_size();
    for (auto range : valid) for (auto [address, bytes] : buffered_byte_spans(backing, layout, geometry, range))
        intervals.emplace_back(buffered_page_key(address, page_size), buffered_page_key(address + bytes - 1, page_size));
    std::ranges::sort(intervals);
    std::vector<BufferedPageInterval> merged;
    for (auto interval : intervals) {
        if (!merged.empty() && interval.first <= add_buffered_pages(merged.back().second, 1))
            merged.back().second = std::max(merged.back().second, interval.second);
        else merged.push_back(interval);
    }
    return merged;
}
void prune_tock_pages(BufferedSampleBacking& backing, ChannelLayout layout,
    ContinuousValueStorageGeometry geometry, IndexCoverage const& touched, IndexCoverage const& valid)
{
    if (valid.empty()) { backing.pages = {}; backing.runs = {}; return; }
    auto const kept = valid_page_intervals(backing, layout, geometry, valid);
    std::vector<BufferedPageKey> removed;
    for (auto [first, last] : valid_page_intervals(backing, layout, geometry, touched)) {
        auto visit = [&](auto&& self, std::shared_ptr<BufferedPageNode const> const& node) -> void {
            if (!node) return;
            if (first < node->key) self(self, node->left);
            if (first <= node->key && node->key <= last) {
                auto after = std::upper_bound(kept.begin(), kept.end(), node->key,
                    [](auto key, auto interval) { return key < interval.first; });
                if (after == kept.begin() || std::prev(after)->second < node->key) removed.push_back(node->key);
            }
            if (node->key < last) self(self, node->right);
        };
        visit(visit, backing.pages);
    }
    for (auto key : removed) {
        backing.pages = erase_buffered_page(backing.pages, key);
        backing.runs = replace_buffered_page_run(backing.runs, key, std::nullopt, backing.allocator->page_size());
    }
}

void write_tock_windows(BufferedSampleBacking& backing, ChannelLayout layout,
    ContinuousValueStorageGeometry geometry, PersistedSampleRegion const* incoming)
{
    if (!incoming) return;
    struct Write { BufferedByteAddress address; std::span<std::byte const> bytes; };
    std::vector<Write> writes;
    auto append = [&](PersistedSampleRegion const& window) {
        std::size_t offset = 0;
        for (auto [address, bytes] : buffered_byte_spans(backing, layout, geometry, window.range)) {
            writes.push_back({address, {window.storage_data() + offset, bytes}});
            offset += bytes;
        }
    };
    if (incoming->windows) for (auto const& window : *incoming->windows) append(window);
    else append(*incoming);
    std::ranges::sort(writes, {}, &Write::address);
    auto const page_size = backing.allocator->page_size();
    std::vector<std::byte> contents(page_size);
    std::optional<BufferedPageKey> pending;
    bool changed = false;
    auto finish = [&] {
        if (!pending || !changed) return;
        auto page = backing.allocator->allocate(contents);
        backing.runs = replace_buffered_page_run(backing.runs, *pending, page->file_offset, page_size);
        backing.pages = put_buffered_page(backing.pages, *pending, std::move(page));
    };
    // Sort spans, then stream one scratch page. Separate incoming windows that
    // touch the same page receive one copy-on-write allocation, not one each.
    for (auto write : writes) while (!write.bytes.empty()) {
        auto const key = buffered_page_key(write.address, page_size);
        if (!pending || *pending != key) {
            finish();
            pending = key;
            auto const* old = find_buffered_page(backing.pages, key);
            changed = !old;
            if (old) backing.allocator->read(*old, contents);
            else std::fill(contents.begin(), contents.end(), std::byte{});
        }
        auto const offset = static_cast<std::size_t>(write.address % page_size);
        auto const count = std::min(write.bytes.size(), page_size - offset);
        changed |= std::memcmp(contents.data() + offset, write.bytes.data(), count) != 0;
        std::memcpy(contents.data() + offset, write.bytes.data(), count);
        write.address += count;
        write.bytes = write.bytes.subspan(count);
    }
    finish();
}

bool unchanged_tock_view(PersistedSampleRegion const& view, BufferedSampleBacking const& backing,
    ChannelLayout layout, ContinuousValueStorageGeometry geometry)
{
    if (!view.buffered_backing || view.buffered_backing->logical_range != backing.logical_range) return false;
    auto const page_size = backing.allocator->page_size();
    for (auto [address, bytes] : buffered_byte_spans(backing, layout, geometry, view.range)) {
        auto const occupied = static_cast<std::size_t>(address % page_size) + bytes;
        auto const count = occupied / page_size + (occupied % page_size != 0);
        if (!same_buffered_runs(view.buffered_backing->runs.get(), backing.runs,
                buffered_page_key(address, page_size), count, page_size)) return false;
    }
    return true;
}

PersistedSampleRegion replace_tock_region(PersistedOutputHandle output, GlobalIndexRange logical,
    ChannelLayout layout, ContinuousValueStorageGeometry geometry, IndexCoverage const& domain,
    PersistedSampleRegion const* prior, PersistedSampleRegion const* incoming,
    std::shared_ptr<BufferedPageAllocator> const& allocator)
{
    auto backing = std::make_shared<BufferedSampleBacking>();
    if (prior) {
        if (!prior->buffered_backing || prior->buffered_backing->logical_range != logical)
            throw std::logic_error("authored Region lost its canonical backing directory");
        *backing = *prior->buffered_backing;
    } else {
        backing->allocator = allocator;
        backing->origin = logical.begin;
        backing->logical_range = logical;
        if (layout.sample_layout == SampleStreamLayout::planar && channel_count(layout) > 1) {
            auto const plane = static_cast<BufferedByteAddress>(logical.extent) * geometry.value_size_bytes;
            auto const page = allocator->page_size();
            backing->plane_stride_bytes = plane * channel_count(layout) <= page ? plane
                : (plane / page + (plane % page != 0)) * page;
        }
    }
    auto const old_valid = prior ? prior->materialized_coverage() : IndexCoverage{};
    auto valid = old_valid - domain;
    if (incoming) valid.include(incoming->materialized_coverage());
    write_tock_windows(*backing, layout, geometry, incoming);
    prune_tock_pages(*backing, layout, geometry, old_valid & domain, valid);
    if (prior && valid == old_valid && backing->pages == prior->buffered_backing->pages) return *prior;

    PersistedSampleRegion result{.output = output, .range = logical, .layout = layout,
        .geometry = geometry, .buffered_backing = backing};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> frames;
    for (auto range : valid) {
        auto const offset = logical.begin.forward_distance_to(range.begin);
        frames.emplace_back(offset, offset + range.extent);
    }
    std::ranges::sort(frames);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
    for (auto interval : frames) {
        if (!merged.empty() && interval.first == merged.back().second) merged.back().second = interval.second;
        else merged.push_back(interval);
    }
    auto const page = allocator->page_size();
    auto const preferred = std::min<std::size_t>(allocator->limits().max_contiguous_mapping_bytes, 1024 * 1024) / page * page;
    auto const frame_bytes = buffered_bytes_per_frame(layout, geometry);
    auto chunk_frames = preferred / frame_bytes;
    if (!chunk_frames && !valid.empty()) throw std::length_error("buffered view limit cannot fit one frame");
    auto const step = page / std::gcd(page, layout.sample_layout == SampleStreamLayout::planar
        ? geometry.value_size_bytes : frame_bytes);
    if (chunk_frames >= step) chunk_frames = chunk_frames / step * step;
    struct PriorView {
        std::pair<std::uint64_t, std::uint64_t> key;
        PersistedSampleRegion const* view;
    };
    std::vector<PriorView> prior_views;
    if (prior) {
        auto remember = [&](PersistedSampleRegion const& view) {
            prior_views.push_back({{logical.begin.forward_distance_to(view.range.begin), view.range.extent}, &view});
        };
        if (prior->windows) for (auto const& view : *prior->windows) remember(view);
        else remember(*prior);
        std::ranges::sort(prior_views, {}, &PriorView::key);
    }
    std::vector<PersistedSampleRegion> windows;
    for (auto [begin, end] : merged) while (begin < end) {
        auto const count = std::min<std::uint64_t>(end - begin, chunk_frames - begin % chunk_frames);
        GlobalIndexRange range{logical.begin.advanced_by(begin), count};
        std::pair<std::uint64_t, std::uint64_t> const key{begin, count};
        auto const cached = std::ranges::lower_bound(prior_views, key, {}, &PriorView::key);
        auto const* old = cached != prior_views.end() && cached->key == key ? cached->view : nullptr;
        if (old && unchanged_tock_view(*old, *backing, layout, geometry)) windows.push_back(*old);
        else windows.push_back(map_buffered_slice(result, range));
        begin += count;
    }
    if (windows.size() == 1 && windows.front().range == logical) result = std::move(windows.front());
    else result.set_prepared_windows(std::move(windows));
    result.buffered_backing = std::move(backing);
    return result;
}

[[nodiscard]] std::shared_ptr<
    std::vector<ContinuousRegionStorageView> const>
make_sample_region_views(
    std::shared_ptr<std::vector<PersistedSampleRegion> const> const& regions)
{
    auto views = std::make_shared<std::vector<ContinuousRegionStorageView>>();
    views->reserve(regions ? regions->size() : 0);
    if (regions) {
        for (auto const& region : *regions) {
            views->push_back(ContinuousRegionStorageView{
                .storage = const_cast<std::byte*>(region.storage_data()),
                .range = region.range,
                .geometry = region.geometry,
                .channel_layout = region.layout,
                .windows = region.window_views
                    ? std::span<ContinuousRegionStorageView const>{*region.window_views}
                    : std::span<ContinuousRegionStorageView const>{},
            });
        }
    }
    return views;
}

template<typename Record>
void remove_range(
    std::shared_ptr<RangeNode<Record> const>& root,
    std::size_t& count,
    GlobalIndexRange removed)
{
    std::vector<std::shared_ptr<Record const>> intersecting_records;
    for_each_intersecting(root, removed, [&](auto const& record) {
        intersecting_records.push_back(record);
    });
    auto const removed_linear = linear_range(removed);
    for (auto const& existing : intersecting_records) {
        auto const existing_linear = linear_range(existing->range);
        root = erase_record(root, existing_linear.begin);
        --count;

        if (existing_linear.begin < removed_linear.begin) {
            auto prefix = GlobalIndexRange{
                .begin = existing->range.begin,
                .extent = removed_linear.begin - existing_linear.begin,
            };
            root = put_record(
                root,
                std::make_shared<Record const>(slice(*existing, prefix)));
            ++count;
        }
        if (less(removed_linear.end, existing_linear.end)) {
            assert(!removed_linear.end.domain_end);
            auto const suffix_extent = existing_linear.end.domain_end
                ? GlobalIndex::extent_type{0} - removed_linear.end.key
                : existing_linear.end.key - removed_linear.end.key;
            auto suffix = GlobalIndexRange{
                .begin = from_signed_key(removed_linear.end.key),
                .extent = suffix_extent,
            };
            root = put_record(
                root,
                std::make_shared<Record const>(slice(*existing, suffix)));
            ++count;
        }
    }
}

constexpr std::uint32_t output_directory_mask = 15;
constexpr std::uint8_t output_directory_digit_bits = 4;

[[nodiscard]] std::uint8_t output_directory_levels(
    std::uint32_t index) noexcept
{
    std::uint8_t result = 0;
    auto remaining = index >> output_directory_digit_bits;
    while (remaining != 0) {
        ++result;
        remaining >>= output_directory_digit_bits;
    }
    return result;
}

[[nodiscard]] std::size_t output_directory_digit(
    std::uint32_t index,
    std::uint8_t level) noexcept
{
    return static_cast<std::size_t>(
        (index >> (level * output_directory_digit_bits))
        & output_directory_mask);
}

[[nodiscard]] bool output_directory_contains(
    std::uint8_t levels,
    std::uint32_t index) noexcept
{
    auto const capacity = std::uint64_t{1}
        << ((static_cast<unsigned>(levels) + 1)
            * output_directory_digit_bits);
    return index < capacity;
}

template<typename Node>
[[nodiscard]] std::shared_ptr<Node const> make_output_leaf(
    typename Node::Values values)
{
    return std::make_shared<Node const>(Node{
        .entries = std::move(values),
    });
}

template<typename Node>
[[nodiscard]] std::shared_ptr<Node const> make_output_branch(
    typename Node::Children children)
{
    return std::make_shared<Node const>(Node{
        .entries = std::move(children),
    });
}

template<typename Node, typename State>
[[nodiscard]] State const* find_output(
    std::shared_ptr<Node const> const& root,
    std::uint8_t levels,
    std::uint32_t index) noexcept
{
    if (!output_directory_contains(levels, index)) return nullptr;
    auto const* node = root.get();
    while (node && levels != 0) {
        auto const* children =
            std::get_if<typename Node::Children>(&node->entries);
        assert(children != nullptr);
        node = (*children)[output_directory_digit(index, levels)].get();
        --levels;
    }
    if (!node) return nullptr;
    auto const* values = std::get_if<typename Node::Values>(&node->entries);
    assert(values != nullptr);
    return (*values)[index & output_directory_mask].get();
}

template<typename Node, typename State>
[[nodiscard]] std::shared_ptr<State const> find_output_owner(
    std::shared_ptr<Node const> root, std::uint8_t levels, std::uint32_t index)
{
    if (!output_directory_contains(levels, index)) return {};
    while (root && levels != 0) {
        auto const& children = std::get<typename Node::Children>(root->entries);
        root = children[output_directory_digit(index, levels--)];
    }
    if (!root) return {};
    return std::get<typename Node::Values>(root->entries)[index & output_directory_mask];
}

template<typename Node, typename State>
[[nodiscard]] std::shared_ptr<Node const> put_output_node(
    std::shared_ptr<Node const> const& root,
    std::uint8_t level,
    std::uint32_t index,
    std::shared_ptr<State const> state)
{
    if (level == 0) {
        typename Node::Values values{};
        if (root) {
            auto const* existing =
                std::get_if<typename Node::Values>(&root->entries);
            assert(existing != nullptr);
            values = *existing;
        }
        values[index & output_directory_mask] = std::move(state);
        return make_output_leaf<Node>(std::move(values));
    }

    typename Node::Children children{};
    if (root) {
        auto const* existing =
            std::get_if<typename Node::Children>(&root->entries);
        assert(existing != nullptr);
        children = *existing;
    }
    auto const digit = output_directory_digit(index, level);
    children[digit] = put_output_node<Node, State>(
        children[digit], level - 1, index, std::move(state));
    return make_output_branch<Node>(std::move(children));
}

template<typename Node, typename State>
[[nodiscard]] std::shared_ptr<Node const> put_output(
    std::shared_ptr<Node const> root,
    std::uint8_t& levels,
    std::uint32_t index,
    std::shared_ptr<State const> state)
{
    auto const required = output_directory_levels(index);
    while (levels < required) {
        if (root) {
            typename Node::Children children{};
            children[0] = std::move(root);
            root = make_output_branch<Node>(std::move(children));
        }
        ++levels;
    }
    return put_output_node<Node, State>(
        root, levels, index, std::move(state));
}

template<typename Entry>
[[nodiscard]] bool output_directory_empty(Entry const& entries) noexcept
{
    return std::ranges::all_of(
        entries, [](auto const& entry) { return !entry; });
}

template<typename Node>
[[nodiscard]] std::shared_ptr<Node const> erase_output_node(
    std::shared_ptr<Node const> const& root,
    std::uint8_t level,
    std::uint32_t index)
{
    if (!root) return {};
    if (level == 0) {
        auto const* existing =
            std::get_if<typename Node::Values>(&root->entries);
        assert(existing != nullptr);
        auto values = *existing;
        values[index & output_directory_mask].reset();
        return output_directory_empty(values)
            ? std::shared_ptr<Node const>{}
            : make_output_leaf<Node>(std::move(values));
    }

    auto const* existing =
        std::get_if<typename Node::Children>(&root->entries);
    assert(existing != nullptr);
    auto children = *existing;
    auto const digit = output_directory_digit(index, level);
    children[digit] = erase_output_node<Node>(
        children[digit], level - 1, index);
    return output_directory_empty(children)
        ? std::shared_ptr<Node const>{}
        : make_output_branch<Node>(std::move(children));
}

} // namespace

PersistedContinuousStorage::PersistedContinuousStorage(
    std::size_t size,
    std::size_t alignment)
    : alignment_(alignment)
{
    if (size == 0 || alignment == 0 || !std::has_single_bit(alignment)) {
        throw std::invalid_argument(
            "persisted continuous storage has invalid geometry");
    }
    storage_ = {
        static_cast<std::byte*>(
            ::operator new(size, std::align_val_t{alignment})),
        Delete{alignment}};
    size_ = size;
    std::fill_n(storage_.get(), size_, std::byte{});
}

PersistedContinuousStorage::PersistedContinuousStorage(
    PersistedContinuousStorage const& source)
    : alignment_(source.alignment_)
{
    if (source.empty()) return;
    if (source.external_storage_) {
        external_owner_ = source.external_owner_;
        external_storage_ = source.external_storage_;
        size_ = source.size_;
        return;
    }
    storage_ = {
        static_cast<std::byte*>(
            ::operator new(source.size_, std::align_val_t{source.alignment_})),
        Delete{source.alignment_}};
    size_ = source.size_;
    std::memcpy(storage_.get(), source.data(), size_);
}

PersistedContinuousStorage& PersistedContinuousStorage::operator=(
    PersistedContinuousStorage const& source)
{
    if (this == &source) return *this;
    if (source.empty()) {
        storage_.reset();
        external_owner_.reset();
        external_storage_ = nullptr;
        size_ = 0;
        alignment_ = alignof(std::max_align_t);
        return *this;
    }
    PersistedContinuousStorage copy{source};
    *this = std::move(copy);
    return *this;
}

PersistedContinuousStorage::PersistedContinuousStorage(
    PersistedContinuousStorage&& source) noexcept
    : storage_(std::move(source.storage_))
    , external_owner_(std::move(source.external_owner_))
    , external_storage_(std::exchange(source.external_storage_, nullptr))
    , size_(std::exchange(source.size_, 0))
    , alignment_(std::exchange(
          source.alignment_, alignof(std::max_align_t)))
{}

PersistedContinuousStorage& PersistedContinuousStorage::operator=(
    PersistedContinuousStorage&& source) noexcept
{
    if (this == &source) return *this;
    storage_ = std::move(source.storage_);
    external_owner_ = std::move(source.external_owner_);
    external_storage_ = std::exchange(source.external_storage_, nullptr);
    size_ = std::exchange(source.size_, 0);
    alignment_ = std::exchange(
        source.alignment_, alignof(std::max_align_t));
    return *this;
}

PersistedContinuousStorage PersistedContinuousStorage::mapped(
    std::shared_ptr<void const> owner,
    std::byte const* storage,
    std::size_t size,
    std::size_t alignment)
{
    if (!owner || !storage || size == 0 || alignment == 0
        || !std::has_single_bit(alignment)
        || reinterpret_cast<std::uintptr_t>(storage) % alignment != 0) {
        throw std::invalid_argument(
            "mapped persisted continuous storage has invalid geometry");
    }
    PersistedContinuousStorage result;
    result.external_owner_ = std::move(owner);
    result.external_storage_ = const_cast<std::byte*>(storage);
    result.size_ = size;
    result.alignment_ = alignment;
    return result;
}

void PersistedSampleRegion::set_prepared_windows(std::vector<PersistedSampleRegion> prepared)
{
    windows = std::make_shared<std::vector<PersistedSampleRegion> const>(std::move(prepared));
    auto views = std::make_shared<std::vector<ContinuousRegionStorageView>>();
    views->reserve(windows->size());
    for (auto const& window : *windows) views->push_back({
        .storage = const_cast<std::byte*>(window.storage_data()),
        .range = window.range, .geometry = window.geometry, .channel_layout = window.layout});
    window_views = std::move(views);
}

PersistedSampleRegion PersistedSampleRegion::contiguous_view() const
{
    if (storage_data()) return *this;
    if (!materialized_coverage().contains(range))
        throw std::runtime_error("complete buffered Region contains unprepared positions");
    if (buffered_backing) return map_buffered_slice(*this, range);
    if (!windows || windows->empty() || !windows->front().buffered_backing)
        throw std::runtime_error("complete buffered Region has no backing owner");
    auto const allocator = windows->front().buffered_backing->allocator;
    auto const frames = checked_frame_count(range);
    auto const channels = channel_count(layout);
    auto const bytes = checked_product(checked_product(frames, channels), geometry.value_size_bytes);
    if (bytes > allocator->limits().max_contiguous_mapping_bytes)
        throw std::length_error("complete buffered Region requires " + std::to_string(bytes)
            + " bytes; contiguous limit " + std::to_string(allocator->limits().max_contiguous_mapping_bytes));
    PersistedSampleRegion contiguous{.output = output, .range = range, .layout = layout, .geometry = geometry,
        .continuous_values = PersistedContinuousStorage{bytes, geometry.element_alignment}};
    for (auto const& window : *windows) {
        auto const offset = static_cast<std::size_t>(range.begin.forward_distance_to(window.range.begin));
        auto const count = checked_frame_count(window.range);
        if (layout.sample_layout == SampleStreamLayout::interleaved)
            std::memcpy(contiguous.storage_data() + offset * channels * geometry.value_size_bytes,
                window.storage_data(), count * channels * geometry.value_size_bytes);
        else for (std::size_t channel = 0; channel < channels; ++channel)
            std::memcpy(contiguous.storage_data() + (channel * frames + offset) * geometry.value_size_bytes,
                window.storage_data() + channel * count * geometry.value_size_bytes, count * geometry.value_size_bytes);
    }
    return page_backed_region(std::move(contiguous), allocator);
}

PersistedSampleRegion PersistedSampleRegion::selected_view(GlobalIndexRange selected) const
{
    if (selected.empty() || !range.contains(selected))
        throw std::invalid_argument("buffered selected view lies outside its Region");
    if (windows) {
        std::vector<PersistedSampleRegion> prepared;
        for (auto const& window : *windows)
            for (auto const part : IndexCoverage{window.range} & IndexCoverage{selected})
                prepared.push_back(window.selected_view(part));
        PersistedSampleRegion result{.output = output, .range = selected, .layout = layout, .geometry = geometry};
        result.set_prepared_windows(std::move(prepared));
        return result;
    }
    if (selected == range) return *this;
    return slice(*this, selected);
}

IndexCoverage PersistedSampleRegion::materialized_coverage() const
{
    if (!windows) return IndexCoverage{range};
    IndexCoverage result;
    for (auto const& window : *windows) result.include(window.range);
    return result;
}

PersistedSampleRegion const* PersistedSampleRegion::prepared_window(GlobalIndex index) const noexcept
{
    if (!range.contains(index)) return nullptr;
    if (!windows) return this;
    for (auto const& window : *windows)
        if (auto const* prepared = window.prepared_window(index)) return prepared;
    return nullptr;
}

std::byte* PersistedSampleRegion::storage_data() noexcept
{
    return values.empty()
        ? continuous_values.data()
        : reinterpret_cast<std::byte*>(values.data());
}

std::byte const* PersistedSampleRegion::storage_data() const noexcept
{
    return values.empty()
        ? continuous_values.data()
        : reinterpret_cast<std::byte const*>(values.data());
}

std::size_t PersistedSampleRegion::storage_size_bytes() const noexcept
{
    return values.empty()
        ? continuous_values.size()
        : values.size() * sizeof(Sample);
}

Sample PersistedSampleRegion::at(
    GlobalIndex index,
    std::size_t channel) const noexcept
{
    if (windows) {
        auto const* window = prepared_window(index);
        IV_ASSERT(window, "persisted Region read lies outside prepared windows");
        return window ? window->at(index, channel) : Sample{};
    }

    auto const frame = range.offset_of(index);
    auto const channels = channel_count(layout);
    if (geometry.value_type != continuous_value_type_id_v<Sample>
        || !frame || channel >= channels
        || range.extent > std::numeric_limits<std::size_t>::max()) {
        return {};
    }
    auto const frames = static_cast<std::size_t>(range.extent);
    auto const frame_index = static_cast<std::size_t>(*frame);
    auto const offset = layout.sample_layout == SampleStreamLayout::planar
        ? channel * frames + frame_index
        : frame_index * channels + channel;
    if (channels != 0
        && frames > std::numeric_limits<std::size_t>::max() / channels) {
        return {};
    }
    if (offset >= frames * channels) return {};
    auto const* samples = reinterpret_cast<Sample const*>(storage_data());
    return samples ? samples[offset] : Sample{};
}

PortKind persisted_output_kind(PersistedOutputId const& output) noexcept
{
    return std::visit([](auto const& identity) { return identity.kind; }, output);
}

PersistedOutputId persisted_output_id(
    graph_jit::BackgroundEvaluationPlan const& plan,
    graph_jit::PortStorageIndex storage,
    std::uint64_t generation)
{
    if (storage >= plan.storage.ports.size()) {
        throw std::invalid_argument("persisted storage index is out of range");
    }
    auto const& planned = plan.storage.ports[storage];
    if (!uses_output_cache(plan, planned, true)
        || !planned.output_port || *planned.output_port >= plan.ports.size()) {
        throw std::invalid_argument(
            "persisted storage has no generation output identity");
    }
    auto const output_port = *planned.output_port;
    auto const& output = plan.ports[output_port];
    if (output.stable_identity) return *output.stable_identity;
    return GenerationLocalPersistedOutputId{
        .generation = generation,
        .port = output_port,
        .kind = planned.kind,
    };
}

PersistedOutputHandle PersistedDataStore::resolve_output(
    PersistedOutputId const& output)
{
    std::lock_guard lock{output_slots_mutex_};
    auto const existing = std::ranges::find_if(
        output_slots_, [&](auto const& registered) {
            return registered.first == output;
        });
    if (existing != output_slots_.end()) return existing->second;

    if (output_slots_.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error(
            "persisted data store output slots are exhausted");
    }
    auto const handle = PersistedOutputHandle{
        output_owner_token_,
        static_cast<std::uint32_t>(output_slots_.size()),
        persisted_output_kind(output)};
    output_slots_.push_back({output, handle});
    return handle;
}

std::uint32_t PersistedDataStore::output_owner_token(
    PersistedOutputHandle output) noexcept
{
    return static_cast<std::uint32_t>(
        output.value_ >> PersistedOutputHandle::owner_shift);
}

std::uint32_t PersistedDataStore::output_directory_index(
    PersistedOutputHandle output) noexcept
{
    return static_cast<std::uint32_t>(
        output.value_ >> PersistedOutputHandle::slot_shift);
}

bool PersistedDataStore::owns_output(
    PersistedOutputHandle output) const noexcept
{
    return output.valid()
        && output_owner_token(output) == output_owner_token_;
}

PersistedSampleRegion const*
PersistedDataStore::Snapshot::find_sample_region(
    PersistedOutputHandle output,
    GlobalIndex index) const noexcept
{
    if (!output.valid() || output.kind() != PortKind::sample
        || PersistedDataStore::output_owner_token(output)
            != output_owner_token_) {
        return nullptr;
    }
    auto const* selected = find_output<SampleOutputNode, SampleOutputState>(
        sample_outputs_,
        sample_output_levels_,
        PersistedDataStore::output_directory_index(output));
    if (!selected) return nullptr;
    if (selected->logical_regions) {
        auto const found = std::ranges::find_if(
            *selected->logical_regions,
            [&](PersistedSampleRegion const& region) {
                return region.prepared_window(index) != nullptr;
            });
        return found == selected->logical_regions->end()
            ? nullptr
            : found->prepared_window(index);
    }
    return find_record(selected->regions, index);
}

PersistedEventChunk const* PersistedDataStore::Snapshot::find_event_chunk(
    PersistedOutputHandle output,
    GlobalIndex index) const noexcept
{
    if (!output.valid() || output.kind() != PortKind::event
        || PersistedDataStore::output_owner_token(output)
            != output_owner_token_) {
        return nullptr;
    }
    auto const* selected = find_output<EventOutputNode, EventOutputState>(
        event_outputs_,
        event_output_levels_,
        PersistedDataStore::output_directory_index(output));
    if (!selected) return nullptr;
    if (selected->logical_regions) {
        auto const found = std::ranges::find_if(
            *selected->logical_regions,
            [&](PersistedEventChunk const& region) {
                return region.range.contains(index)
                    && (!region.prepared_coverage || region.prepared_coverage->contains(index));
            });
        return found == selected->logical_regions->end()
            ? nullptr
            : std::addressof(*found);
    }
    return find_record(selected->chunks, index);
}

void PersistedDataStore::Snapshot::for_each_sample_region(
    PersistedOutputHandle output,
    GlobalIndexRange range,
    void* visitor_data,
    VisitSampleRegion visitor) const
{
    if (!visitor || range.empty() || !output.valid()
        || output.kind() != PortKind::sample
        || PersistedDataStore::output_owner_token(output)
            != output_owner_token_) {
        return;
    }
    auto const* selected = find_output<SampleOutputNode, SampleOutputState>(
        sample_outputs_,
        sample_output_levels_,
        PersistedDataStore::output_directory_index(output));
    if (!selected || !selected->logical_regions) return;
    auto visit_window = [&](PersistedSampleRegion const& window) {
        range.for_each_signed_order_segment([&](GlobalIndexRange requested) {
            window.range.for_each_signed_order_segment([&](GlobalIndexRange stored) {
                auto const part = intersection(stored, requested);
                if (!part.empty()) visitor(visitor_data, window, part);
            });
        });
    };
    for (auto const& record : *selected->logical_regions) {
        if (record.windows) for (auto const& window : *record.windows) visit_window(window);
        else visit_window(record);
    }
}

std::span<PersistedSampleRegion const>
PersistedDataStore::Snapshot::sample_regions(
    PersistedOutputHandle output,
    ChannelLayout layout) const noexcept
{
    if (!output.valid() || output.kind() != PortKind::sample
        || PersistedDataStore::output_owner_token(output)
            != output_owner_token_) {
        return {};
    }
    auto const* selected = find_output<SampleOutputNode, SampleOutputState>(
        sample_outputs_,
        sample_output_levels_,
        PersistedDataStore::output_directory_index(output));
    return selected && selected->layout == layout && selected->logical_regions
        ? std::span<PersistedSampleRegion const>{*selected->logical_regions}
        : std::span<PersistedSampleRegion const>{};
}

std::span<ContinuousRegionStorageView const>
PersistedDataStore::Snapshot::sample_region_views(
    PersistedOutputHandle output,
    ChannelLayout layout,
    ContinuousValueStorageGeometry geometry) const noexcept
{
    if (!output.valid() || output.kind() != PortKind::sample
        || PersistedDataStore::output_owner_token(output)
            != output_owner_token_) {
        return {};
    }
    auto const* selected = find_output<SampleOutputNode, SampleOutputState>(
        sample_outputs_,
        sample_output_levels_,
        PersistedDataStore::output_directory_index(output));
    return selected && selected->layout == layout
            && selected->geometry == geometry && selected->region_views
        ? std::span<ContinuousRegionStorageView const>{*selected->region_views}
        : std::span<ContinuousRegionStorageView const>{};
}

void PersistedDataStore::Snapshot::for_each_event_chunk(
    PersistedOutputHandle output,
    GlobalIndexRange range,
    void* visitor_data,
    VisitEventChunk visitor) const
{
    if (!visitor || range.empty() || !output.valid()
        || output.kind() != PortKind::event
        || PersistedDataStore::output_owner_token(output)
            != output_owner_token_) {
        return;
    }
    auto const* selected = find_output<EventOutputNode, EventOutputState>(
        event_outputs_,
        event_output_levels_,
        PersistedDataStore::output_directory_index(output));
    if (!selected) return;
    if (selected->logical_regions) {
        for (auto const& record : *selected->logical_regions) {
            range.for_each_signed_order_segment(
                [&](GlobalIndexRange requested) {
                record.range.for_each_signed_order_segment(
                    [&](GlobalIndexRange stored) {
                    auto const part = intersection(stored, requested);
                    if (!part.empty()) {
                        visitor(visitor_data, record, part);
                    }
                });
            });
        }
        return;
    }
    range.for_each_signed_order_segment([&](GlobalIndexRange segment) {
        for_each_intersecting(selected->chunks, segment, [&](auto const& record) {
            visitor(visitor_data, *record, intersection(record->range, segment));
        });
    });
}

std::span<PersistedEventChunk const>
PersistedDataStore::Snapshot::event_regions(
    PersistedOutputHandle output,
    EventTypeId type) const noexcept
{
    if (!output.valid() || output.kind() != PortKind::event
        || PersistedDataStore::output_owner_token(output)
            != output_owner_token_) {
        return {};
    }
    auto const* selected = find_output<EventOutputNode, EventOutputState>(
        event_outputs_,
        event_output_levels_,
        PersistedDataStore::output_directory_index(output));
    return selected && selected->type == type && selected->logical_regions
        ? std::span<PersistedEventChunk const>{*selected->logical_regions}
        : std::span<PersistedEventChunk const>{};
}

std::span<GlobalIndexRange const>
PersistedDataStore::Snapshot::event_region_ranges(
    PersistedOutputHandle output,
    EventTypeId type) const noexcept
{
    if (!output.valid() || output.kind() != PortKind::event
        || PersistedDataStore::output_owner_token(output)
            != output_owner_token_) {
        return {};
    }
    auto const* selected = find_output<EventOutputNode, EventOutputState>(
        event_outputs_,
        event_output_levels_,
        PersistedDataStore::output_directory_index(output));
    return selected && selected->type == type && selected->region_ranges
        ? std::span<GlobalIndexRange const>{*selected->region_ranges}
        : std::span<GlobalIndexRange const>{};
}

std::span<TimedEvent const>
PersistedDataStore::Snapshot::event_timeline(
    PersistedOutputHandle output,
    EventTypeId type) const noexcept
{
    if (!output.valid() || output.kind() != PortKind::event
        || PersistedDataStore::output_owner_token(output)
            != output_owner_token_) {
        return {};
    }
    auto const* selected = find_output<EventOutputNode, EventOutputState>(
        event_outputs_,
        event_output_levels_,
        PersistedDataStore::output_directory_index(output));
    return selected && selected->type == type && selected->timeline
        ? std::span<TimedEvent const>{*selected->timeline}
        : std::span<TimedEvent const>{};
}

EventNeighbors PersistedDataStore::Snapshot::event_neighbors(
    PersistedOutputHandle output, EventTypeId type, std::size_t region_index,
    GlobalIndex position, EventNeighborMode mode) const
{
    auto const ranges = event_region_ranges(output, type);
    if (region_index >= ranges.size()
        || (mode == EventNeighborMode::bounded && !ranges[region_index].contains(position))) return {};
    auto const region = ranges[region_index];
    // Authored logical Regions own independent, range-relative sequences.
    auto const* selected = find_output<EventOutputNode, EventOutputState>(
        event_outputs_, event_output_levels_, PersistedDataStore::output_directory_index(output));
    if (!selected) return {};
    if (!selected->positional_recording) {
        if (!selected->logical_regions || region_index >= selected->logical_regions->size()) return {};
        return sorted_event_neighbors((*selected->logical_regions)[region_index].events,
            region.begin, position, mode);
    }
    // Positional recordings have one signed-sorted timeline. A circular Region
    // intersects at most two signed-order spans; each is searched separately.
    EventNeighbors result;
    std::optional<TimedEvent> first_event, last_event;
    auto timeline = event_timeline(output, type);
    region.for_each_signed_order_segment([&](GlobalIndexRange segment) {
        auto const first = std::ranges::lower_bound(timeline, segment.begin,
            SignedGlobalIndexOrder{}, &TimedEvent::time);
        auto last = first;
        // The end can be the signed seam, where a signed lower_bound would
        // select the start of the timeline instead of the end.
        if (segment.begin.signed_value() >= 0 && segment.end().signed_value() < 0) last = timeline.end();
        else last = std::ranges::lower_bound(timeline, segment.end(), SignedGlobalIndexOrder{}, &TimedEvent::time);
        if (mode == EventNeighborMode::cyclic && first != last) {
            if (!first_event) first_event = *first;
            last_event = *std::prev(last);
        }
        auto found = sorted_event_neighbors(std::span<TimedEvent const>{first, last}, region.begin, position);
        if (found.at_or_before && (!result.at_or_before
            || region.begin.forward_distance_to(found.at_or_before->time)
                >= region.begin.forward_distance_to(result.at_or_before->time))) result.at_or_before = found.at_or_before;
        if (found.after && (!result.after || region.begin.forward_distance_to(found.after->time)
                < region.begin.forward_distance_to(result.after->time))) result.after = found.after;
    });
    if (mode == EventNeighborMode::cyclic) {
        if (!result.at_or_before) result.at_or_before = last_event;
        if (!result.after) result.after = first_event;
    }
    return result;
}

void PersistedDataStore::Snapshot::for_each_event_region(
    PersistedOutputHandle output,
    EventTypeId type,
    std::size_t region_index,
    GlobalIndexRange requested,
    void* visitor_data,
    VisitEvent visitor) const
{
    if (!visitor || requested.empty() || !output.valid()
        || output.kind() != PortKind::event
        || PersistedDataStore::output_owner_token(output)
            != output_owner_token_) {
        return;
    }
    auto const* selected = find_output<EventOutputNode, EventOutputState>(
        event_outputs_,
        event_output_levels_,
        PersistedDataStore::output_directory_index(output));
    if (!selected || selected->type != type || !selected->region_ranges
        || region_index >= selected->region_ranges->size()
        || !(*selected->region_ranges)[region_index].contains(requested)) {
        return;
    }

    if (!selected->positional_recording) {
        if (!selected->logical_regions
            || region_index >= selected->logical_regions->size()) {
            return;
        }
        auto const& region = (*selected->logical_regions)[region_index];
        for (auto const& event : sorted_event_range(region.events,
                 (*selected->region_ranges)[region_index], requested)) visitor(visitor_data, event);
        return;
    }

    for_each_event_chunk(
        output,
        requested,
        [&](PersistedEventChunk const& chunk, GlobalIndexRange intersection) {
            for (auto const& event : chunk.events) {
                if (intersection.contains(event.time)) {
                    visitor(visitor_data, event);
                }
            }
        });
}

IndexCoverage const* PersistedDataStore::Snapshot::find_event_logical_coverage(
    PersistedOutputHandle output) const noexcept
{
    if (!output.valid() || output.kind() != PortKind::event
        || PersistedDataStore::output_owner_token(output) != output_owner_token_) return nullptr;
    auto const* selected = find_output<EventOutputNode, EventOutputState>(
        event_outputs_, event_output_levels_, PersistedDataStore::output_directory_index(output));
    return selected ? (selected->logical_coverage ? &*selected->logical_coverage : &selected->coverage) : nullptr;
}

std::shared_ptr<void const> PersistedDataStore::Snapshot::sample_output_owner(
    PersistedOutputHandle output) const
{
    if (!output.valid() || output.kind() != PortKind::sample
        || PersistedDataStore::output_owner_token(output) != output_owner_token_) return {};
    return find_output_owner<SampleOutputNode, SampleOutputState>(sample_outputs_, sample_output_levels_,
        PersistedDataStore::output_directory_index(output));
}

IndexCoverage const* PersistedDataStore::Snapshot::find_sample_logical_coverage(
    PersistedOutputHandle output) const noexcept
{
    if (!output.valid() || output.kind() != PortKind::sample
        || PersistedDataStore::output_owner_token(output) != output_owner_token_) return nullptr;
    auto const* selected = find_output<SampleOutputNode, SampleOutputState>(
        sample_outputs_, sample_output_levels_, PersistedDataStore::output_directory_index(output));
    return selected ? (selected->logical_coverage ? &*selected->logical_coverage : &selected->coverage) : nullptr;
}

IndexCoverage const* PersistedDataStore::Snapshot::find_sample_coverage(
    PersistedOutputHandle output,
    ChannelLayout layout) const noexcept
{
    if (!output.valid() || output.kind() != PortKind::sample
        || PersistedDataStore::output_owner_token(output)
            != output_owner_token_) {
        return nullptr;
    }
    auto const* selected = find_output<SampleOutputNode, SampleOutputState>(
        sample_outputs_,
        sample_output_levels_,
        PersistedDataStore::output_directory_index(output));
    return selected && selected->layout == layout
        ? &selected->coverage
        : nullptr;
}

IndexCoverage const* PersistedDataStore::Snapshot::find_sample_coverage(
    PersistedOutputHandle output,
    ChannelLayout layout,
    ContinuousValueStorageGeometry geometry) const noexcept
{
    if (!output.valid() || output.kind() != PortKind::sample
        || PersistedDataStore::output_owner_token(output)
            != output_owner_token_) {
        return nullptr;
    }
    auto const* selected = find_output<SampleOutputNode, SampleOutputState>(
        sample_outputs_,
        sample_output_levels_,
        PersistedDataStore::output_directory_index(output));
    return selected && selected->layout == layout
            && selected->geometry == geometry
        ? &selected->coverage
        : nullptr;
}

IndexCoverage const* PersistedDataStore::Snapshot::find_event_coverage(
    PersistedOutputHandle output,
    EventTypeId type) const noexcept
{
    if (!output.valid() || output.kind() != PortKind::event
        || PersistedDataStore::output_owner_token(output)
            != output_owner_token_) {
        return nullptr;
    }
    auto const* selected = find_output<EventOutputNode, EventOutputState>(
        event_outputs_,
        event_output_levels_,
        PersistedDataStore::output_directory_index(output));
    return selected && selected->type == type
        ? &selected->coverage
        : nullptr;
}

PersistedDataStore::Candidate::Candidate(
    PersistedDataStore& store,
    Snapshot const& base,
    std::unique_ptr<Snapshot> successor) noexcept
    : store_(&store)
    , base_version_(base.version())
    , successor_(std::move(successor))
    , base_identity_(base.logical_identity_)
{}

PersistedDataVersion
PersistedDataStore::Candidate::target_version() const noexcept
{
    return successor_ ? successor_->version() : PersistedDataVersion{};
}

PersistedDataStore::Snapshot const&
PersistedDataStore::Candidate::working_snapshot() const
{
    if (!successor_) throw std::logic_error("persisted data candidate is empty");
    return *successor_;
}

namespace {
template<class Node, class Transform>
std::shared_ptr<Node const> transform_outputs(
    std::shared_ptr<Node const> const& root, Transform const& transform)
{
    if (!root) return {};
    if (auto const* entries = std::get_if<typename Node::Values>(&root->entries)) {
        auto values = *entries;
        bool changed = false;
        for (auto& value : values) if (value) {
            auto next = transform(value);
            changed |= next != value;
            value = std::move(next);
        }
        return changed ? make_output_leaf<Node>(std::move(values)) : root;
    }
    auto children = std::get<typename Node::Children>(root->entries);
    bool changed = false;
    for (auto& child : children) {
        auto next = transform_outputs(child, transform);
        changed |= next != child;
        child = std::move(next);
    }
    return changed ? make_output_branch<Node>(std::move(children)) : root;
}
}

void PersistedDataStore::Snapshot::retain_payload(
    std::span<PersistedPayloadSelection const> selections, bool preserve_recordings)
{
    auto requested = [&](PersistedOutputHandle output) {
        IndexCoverage result;
        for (auto const& selection : selections)
            if (selection.output == output) result.include(selection.coverage);
        return result;
    };
    sample_outputs_ = transform_outputs(sample_outputs_, [&](auto const& source)
        -> std::shared_ptr<SampleOutputState const> {
        if (preserve_recordings && source->positional_recording) return source;
        auto const selected = requested(source->output) & source->coverage;
        if (selected == source->coverage) return source;
        auto result = std::make_shared<SampleOutputState>(*source);
        result->logical_coverage = source->logical_coverage.value_or(source->coverage);
        result->coverage = selected;
        result->regions.reset();
        result->buffered_pages.reset();
        result->recording_runs.reset();
        result->recording_allocator.reset();
        result->physical_region_count = 0;
        auto logical = std::make_shared<std::vector<PersistedSampleRegion>>();
        for (auto const& region : *source->logical_regions) {
            PersistedSampleRegion retained{.output = region.output, .range = region.range,
                .layout = region.layout, .geometry = region.geometry};
            std::vector<PersistedSampleRegion> windows;
            auto append = [&](PersistedSampleRegion const& window) {
                for (auto part : selected & IndexCoverage{window.range}) {
                    windows.push_back(part == window.range ? window : window.selected_view(part));
                    ++result->physical_region_count;
                    part.for_each_signed_order_segment([&](GlobalIndexRange segment) {
                        result->regions = put_record(result->regions,
                            std::make_shared<PersistedSampleRegion const>(
                                segment == part ? windows.back() : slice(windows.back(), segment)));
                    });
                }
            };
            if (region.windows) for (auto const& window : *region.windows) append(window);
            else if (region.storage_data()) append(region);
            retained.set_prepared_windows(std::move(windows));
            if (region.buffered_backing) {
                auto backing = std::make_shared<BufferedSampleBacking>(*region.buffered_backing);
                prune_tock_pages(*backing, region.layout, region.geometry,
                    region.materialized_coverage(), selected & IndexCoverage{region.range});
                retained.buffered_backing = std::move(backing);
            }
            logical->push_back(std::move(retained));
        }
        result->logical_regions = std::move(logical);
        result->region_views = make_sample_region_views(result->logical_regions);
        return result;
    });
    event_outputs_ = transform_outputs(event_outputs_, [&](auto const& source)
        -> std::shared_ptr<EventOutputState const> {
        if (preserve_recordings && source->positional_recording) return source;
        // Immutable event chunks are the retention unit. Unlike fixed-width
        // sample pages they cannot be split without copying event payload.
        if (!(requested(source->output) & source->coverage).empty()) return source;
        if (source->coverage.empty()) return source;
        auto result = std::make_shared<EventOutputState>(*source);
        result->logical_coverage = source->logical_coverage.value_or(source->coverage);
        result->coverage = {};
        result->chunks.reset();
        result->timeline.reset();
        result->timeline_accounting.reset();
        auto logical = std::make_shared<std::vector<PersistedEventChunk>>();
        for (auto const& region : *source->logical_regions)
            logical->push_back({.output = region.output, .range = region.range,
                .type = region.type, .prepared_coverage = IndexCoverage{}});
        result->logical_regions = std::move(logical);
        return result;
    });
}

void PersistedDataStore::ReaderPin::retain_payload(
    std::span<PersistedPayloadSelection const> selected, bool preserve_recordings)
{
    detach();
    owned_snapshot_->retain_payload(selected, preserve_recordings);
}

void PersistedDataStore::ReaderPin::detach()
{
    assert(snapshot_);
    if (!owned_snapshot_) {
        owned_snapshot_ = std::make_unique<Snapshot>(*snapshot_);
        snapshot_ = owned_snapshot_.get();
        slot_->pinned.store(nullptr, std::memory_order_seq_cst);
        slot_ = nullptr;
    }
}

void PersistedDataStore::ReaderPin::select_outputs(std::span<PersistedOutputHandle const> outputs)
{
    detach();
    owned_snapshot_->select_outputs(outputs);
}

void PersistedDataStore::Snapshot::copy_identity_from(Snapshot const& source) noexcept
{
    assert(!sample_outputs_ && !event_outputs_ && !logical_identity_);
    version_ = source.version_;
    output_owner_token_ = source.output_owner_token_;
    logical_identity_ = source.logical_identity_;
}

void PersistedDataStore::Snapshot::select_outputs(std::span<PersistedOutputHandle const> outputs)
{
    auto samples = std::move(sample_outputs_);
    auto events = std::move(event_outputs_);
    auto const sample_levels = sample_output_levels_, event_levels = event_output_levels_;
    sample_region_count_ = event_chunk_count_ = 0;
    sample_output_levels_ = event_output_levels_ = 0;
    std::vector<PersistedOutputHandle> copied;
    for (auto output : outputs) {
        if (!output.valid() || PersistedDataStore::output_owner_token(output) != output_owner_token_
            || std::ranges::contains(copied, output)) continue;
        copied.push_back(output);
        auto index = PersistedDataStore::output_directory_index(output);
        if (output.kind() == PortKind::sample) {
            auto state = find_output_owner<SampleOutputNode, SampleOutputState>(samples, sample_levels, index);
            if (!state) continue;
            sample_region_count_ += state->region_count;
            sample_outputs_ = put_output<SampleOutputNode, SampleOutputState>(
                sample_outputs_, sample_output_levels_, index, std::move(state));
        } else {
            auto state = find_output_owner<EventOutputNode, EventOutputState>(events, event_levels, index);
            if (!state) continue;
            event_chunk_count_ += state->chunk_count;
            event_outputs_ = put_output<EventOutputNode, EventOutputState>(
                event_outputs_, event_output_levels_, index, std::move(state));
        }
    }
}

std::shared_ptr<PersistedDataStore::Reconstruction const>
PersistedDataStore::Snapshot::reconstruction(PersistedOutputHandle output) const
{
    if (PersistedDataStore::output_owner_token(output) != output_owner_token_) return {};
    auto index = PersistedDataStore::output_directory_index(output);
    if (output.kind() == PortKind::sample) {
        auto state = find_output<SampleOutputNode, SampleOutputState>(sample_outputs_, sample_output_levels_, index);
        return state ? state->reconstruction : nullptr;
    }
    auto state = find_output<EventOutputNode, EventOutputState>(event_outputs_, event_output_levels_, index);
    return state ? state->reconstruction : nullptr;
}

void PersistedDataStore::Candidate::set_reconstruction(PersistedOutputHandle output,
    std::shared_ptr<Reconstruction const> context, Snapshot const& previous)
{
    if (!successor_ || !store_->owns_output(output) || !context || !context->inputs)
        throw std::invalid_argument("reconstruction requires an output and immutable inputs");
    auto const old_context = previous.reconstruction(output);
    bool same = old_context && old_context->executable == context->executable
        && old_context->dependencies == context->dependencies;
    if (same) for (auto dependency : context->dependencies)
        same = same && old_context->inputs->shares_output_with(*context->inputs, dependency);
    auto same_regions = [](auto const* old, auto const& next) {
        if (!old || old->logical_coverage != next.logical_coverage
            || !old->logical_regions || !next.logical_regions
            || old->logical_regions->size() != next.logical_regions->size()) return false;
        for (std::size_t index = 0; index < next.logical_regions->size(); ++index)
            if ((*old->logical_regions)[index].range != (*next.logical_regions)[index].range) return false;
        return true;
    };
    auto index = PersistedDataStore::output_directory_index(output);
    if (output.kind() == PortKind::sample) {
        auto owner = find_output_owner<SampleOutputNode, SampleOutputState>(successor_->sample_outputs_, successor_->sample_output_levels_, index);
        if (!owner || owner->positional_recording) return;
        auto next = std::make_shared<SampleOutputState>(*owner);
        auto old = find_output<SampleOutputNode, SampleOutputState>(previous.sample_outputs_, previous.sample_output_levels_, index);
        if (same && same_regions(old, *owner)) next->semantic_identity = old->semantic_identity;
        else next->semantic_identity = std::make_shared<std::byte>();
        next->reconstruction = std::move(context);
        successor_->sample_outputs_ = put_output<SampleOutputNode, SampleOutputState>(successor_->sample_outputs_, successor_->sample_output_levels_, index, std::move(next));
    } else {
        auto owner = find_output_owner<EventOutputNode, EventOutputState>(successor_->event_outputs_, successor_->event_output_levels_, index);
        if (!owner || owner->positional_recording) return;
        auto next = std::make_shared<EventOutputState>(*owner);
        auto old = find_output<EventOutputNode, EventOutputState>(previous.event_outputs_, previous.event_output_levels_, index);
        if (same && same_regions(old, *owner)) next->semantic_identity = old->semantic_identity;
        else next->semantic_identity = std::make_shared<std::byte>();
        next->reconstruction = std::move(context);
        successor_->event_outputs_ = put_output<EventOutputNode, EventOutputState>(successor_->event_outputs_, successor_->event_output_levels_, index, std::move(next));
    }
}

void PersistedDataStore::Candidate::retain_payload(
    std::span<PersistedPayloadSelection const> selected)
{
    if (!successor_) throw std::logic_error("persisted data candidate is empty");
    successor_->retain_payload(selected, true);
}

void PersistedDataStore::reclaim_cached_payload(
    std::span<PersistedPayloadSelection const> protected_payload)
{
    auto next = std::make_unique<Snapshot>(*published_owner_);
    next->retain_payload(protected_payload, true);
    if (next->sample_outputs_ == published_owner_->sample_outputs_
        && next->event_outputs_ == published_owner_->event_outputs_) return;
    retired_.push_back(std::move(published_owner_));
    published_owner_ = std::move(next);
    published_.store(published_owner_.get(), std::memory_order_seq_cst);
    ++cache_evictions_;
    (void)reclaim_retired();
}

void PersistedDataStore::set_cache_realization(
    PersistedOutputHandle output, OutputRealization realization)
{
    if (!owns_output(output)) throw std::invalid_argument("cache output belongs to another store");
    auto& policy = cache_policy_[output];
    policy.realization = realization;
    pending_computed_reclamation_ |= realization == OutputRealization::computed && policy.uses < 2;
}

void PersistedDataStore::prepare_cache(std::size_t incoming_bytes,
    std::span<PersistedPayloadSelection const> required,
    std::function<void(std::span<PersistedPayloadSelection const>)> const& release_leases)
{
    auto const& limits = buffered_pages_->limits();
    struct Victim { PersistedOutputHandle output; std::size_t bytes; CachePolicy policy; };
    std::vector<Victim> victims;
    std::vector<PersistedPayloadSelection> keep;
    std::size_t retained_bytes = 0;
    std::vector<PersistedOutputHandle> touched;
    for (auto const& selection : required) {
        if (selection.coverage.empty() || std::ranges::contains(touched, selection.output)) continue;
        touched.push_back(selection.output);
        auto& policy = cache_policy_[selection.output];
        policy.last_use = ++cache_clock_;
        ++policy.uses;
    }
    auto const usage = buffered_pages_->usage();
    auto const live = usage.live_page_bytes + usage.event_bytes;
    auto mapping_pressure = [&](BufferedStorageUsage const& current) {
        return current.reserved_mapping_bytes > limits.max_total_mapping_bytes - limits.max_total_mapping_bytes / 4
            || current.mapped_runs > limits.max_mapping_runs - limits.max_mapping_runs / 4;
    };
    if (!pending_computed_reclamation_ && live <= limits.max_cached_payload_bytes
        && incoming_bytes <= limits.max_backing_bytes
        && live <= limits.max_backing_bytes - incoming_bytes && !mapping_pressure(usage)) return;
    if (required.empty()) pending_computed_reclamation_ = false;
    auto requested = [&](PersistedOutputHandle output) {
        IndexCoverage result;
        for (auto const& selection : required)
            if (selection.output == output) result.include(selection.coverage);
        return result;
    };
    auto admit = [&](auto const& source, std::size_t bytes) {
        if (source->positional_recording || source->coverage.empty()) return;
        auto& policy = cache_policy_[source->output];
        auto demand = requested(source->output);
        keep.push_back({source->output, source->coverage});
        retained_bytes += bytes;
        if (!(source->coverage - demand).empty()) victims.push_back({source->output, bytes, policy});
    };
    (void)transform_outputs(published_owner_->sample_outputs_, [&](auto const& source) {
        if (source->positional_recording) return source;
        std::size_t bytes = 0;
        // A conservative page-rounded footprint, including independent
        // overlapping Regions. Hard allocation limits use allocator accounting.
        for (auto const& region : *source->logical_regions)
            for (auto range : region.materialized_coverage()) {
                auto const width = source->geometry.value_size_bytes * channel_count(source->layout);
                auto const page = buffered_pages_->page_size();
                auto const count = checked_product(checked_frame_count(range), width);
                bytes += count / page * page + (count % page ? page : 0);
            }
        admit(source, bytes);
        return source;
    });
    (void)transform_outputs(published_owner_->event_outputs_, [&](auto const& source) {
        if (source->positional_recording) return source;
        std::size_t bytes = source->timeline ? source->timeline->capacity() * sizeof(TimedEvent) : 0;
        for (auto const& region : *source->logical_regions) bytes += region.events.capacity() * sizeof(TimedEvent);
        admit(source, bytes);
        return source;
    });
    std::stable_sort(victims.begin(), victims.end(), [](auto const& left, auto const& right) {
        if (left.policy.realization != right.policy.realization)
            return left.policy.realization == OutputRealization::computed;
        if (left.policy.last_use != right.policy.last_use) return left.policy.last_use < right.policy.last_use;
        if (left.policy.uses != right.policy.uses) return left.policy.uses < right.policy.uses;
        return left.bytes > right.bytes;
    });
    auto pressure = [&] {
        auto const usage = buffered_pages_->usage();
        auto const live = usage.live_page_bytes + usage.event_bytes;
        return retained_bytes > limits.max_cached_payload_bytes
            || incoming_bytes > limits.max_backing_bytes
            || live > limits.max_backing_bytes - incoming_bytes || mapping_pressure(usage);
    };
    for (auto const& victim : victims) {
        // Computed results are not admitted to optional retention on first
        // use. Repeated reads establish reuse; buffered results favor caching.
        bool const discard_computed = victim.policy.realization == OutputRealization::computed
            && victim.policy.uses < 2;
        if (!discard_computed && !pressure()) continue;
        auto found = std::ranges::find(keep, victim.output, &PersistedPayloadSelection::output);
        found->coverage = requested(victim.output);
        release_leases(keep);
        reclaim_cached_payload(keep);
        retained_bytes -= std::min(retained_bytes, victim.bytes);
    }
}

bool PersistedDataStore::Snapshot::shares_output_with(
    Snapshot const& other, PersistedOutputHandle output) const noexcept
{
    if (output_owner_token_ != other.output_owner_token_ || !output.valid()
        || PersistedDataStore::output_owner_token(output) != output_owner_token_) return false;
    auto const index = PersistedDataStore::output_directory_index(output);
    auto same_version = [](auto const* left, auto const* right) {
        return left && right ? left->semantic_identity == right->semantic_identity
            : left == right;
    };
    if (output.kind() == PortKind::sample)
        return same_version(
            find_output<SampleOutputNode, SampleOutputState>(sample_outputs_, sample_output_levels_, index),
            find_output<SampleOutputNode, SampleOutputState>(other.sample_outputs_, other.sample_output_levels_, index));
    return same_version(
        find_output<EventOutputNode, EventOutputState>(event_outputs_, event_output_levels_, index),
        find_output<EventOutputNode, EventOutputState>(other.event_outputs_, other.event_output_levels_, index));
}

void PersistedDataStore::Candidate::reuse_output(Snapshot const& source, PersistedOutputHandle output)
{
    if (!successor_ || !store_->owns_output(output)
        || source.output_owner_token_ != successor_->output_owner_token_)
        throw std::invalid_argument("reused output does not belong to the candidate's data store");
    auto const index = PersistedDataStore::output_directory_index(output);
    if (output.kind() == PortKind::sample) {
        auto const* from = find_output<SampleOutputNode, SampleOutputState>(source.sample_outputs_, source.sample_output_levels_, index);
        auto const* old = find_output<SampleOutputNode, SampleOutputState>(successor_->sample_outputs_, successor_->sample_output_levels_, index);
        if (from == old) return;
        if (!from) { erase_output(output); return; }
        successor_->sample_region_count_ = successor_->sample_region_count_ - (old ? old->region_count : 0) + from->region_count;
        successor_->sample_outputs_ = put_output<SampleOutputNode, SampleOutputState>(
            successor_->sample_outputs_, successor_->sample_output_levels_, index,
            find_output_owner<SampleOutputNode, SampleOutputState>(source.sample_outputs_, source.sample_output_levels_, index));
    } else {
        auto const* from = find_output<EventOutputNode, EventOutputState>(source.event_outputs_, source.event_output_levels_, index);
        auto const* old = find_output<EventOutputNode, EventOutputState>(successor_->event_outputs_, successor_->event_output_levels_, index);
        if (from == old) return;
        if (!from) { erase_output(output); return; }
        successor_->event_chunk_count_ = successor_->event_chunk_count_ - (old ? old->chunk_count : 0) + from->chunk_count;
        successor_->event_outputs_ = put_output<EventOutputNode, EventOutputState>(
            successor_->event_outputs_, successor_->event_output_levels_, index,
            find_output_owner<EventOutputNode, EventOutputState>(source.event_outputs_, source.event_output_levels_, index));
    }
}

void PersistedDataStore::Candidate::overwrite(PersistedSampleRegion record)
{
    if (!successor_) throw std::logic_error("persisted data candidate is empty");
    validate(record);
    if (!store_->owns_output(record.output)) {
        throw std::invalid_argument(
            "sample region output belongs to another persisted data store");
    }

    auto const output_index =
        PersistedDataStore::output_directory_index(record.output);
    auto const* current = find_output<SampleOutputNode, SampleOutputState>(
        successor_->sample_outputs_,
        successor_->sample_output_levels_,
        output_index);
    if (current && (current->layout != record.layout
            || current->geometry != record.geometry)) {
        throw std::invalid_argument(
            "persisted sample output regions disagree on layout or geometry");
    }
    if (record.layout.sample_layout != SampleStreamLayout::interleaved) {
        throw std::invalid_argument(
            "persisted Tick continuous recording requires interleaved storage");
    }
    if (current && !current->positional_recording) {
        throw std::invalid_argument(
            "persisted sample output cannot mix authored Regions with positional recording");
    }

    auto allocator = store_->buffered_pages_;
    auto pages = current ? current->buffered_pages
                         : std::shared_ptr<BufferedPageNode const>{};
    auto runs = current ? current->recording_runs
                        : std::shared_ptr<BufferedPageRunNode const>{};
    write_recording_region(pages, runs, *allocator, record);
    auto coverage = current ? current->coverage : IndexCoverage{};
    coverage.include(record.range);
    auto logical_const = make_recording_regions(
        record.output,
        coverage,
        record.layout,
        record.geometry,
        allocator,
        runs,
        pages,
        current ? current->logical_regions : nullptr);
    auto region_views = make_sample_region_views(logical_const);

    auto const previous_count = current ? current->region_count : 0;
    auto const logical_count = logical_const->size();
    successor_->sample_region_count_ =
        successor_->sample_region_count_ - previous_count + logical_count;
    auto state = std::make_shared<SampleOutputState const>(SampleOutputState{
        .output = record.output,
        .coverage = std::move(coverage),
        .layout = record.layout,
        .geometry = record.geometry,
        .logical_regions = std::move(logical_const),
        .region_views = std::move(region_views),
        .recording_allocator = std::move(allocator),
        .buffered_pages = std::move(pages),
        .recording_runs = std::move(runs),
        .positional_recording = true,
        .region_count = logical_count,
    });
    successor_->sample_outputs_ = put_output<
        SampleOutputNode, SampleOutputState>(
            successor_->sample_outputs_,
            successor_->sample_output_levels_,
            output_index,
            std::move(state));
}

void PersistedDataStore::Candidate::overwrite(PersistedEventChunk record)
{
    if (!successor_) throw std::logic_error("persisted data candidate is empty");
    validate(record);
    if (!store_->owns_output(record.output)) {
        throw std::invalid_argument(
            "event chunk output belongs to another persisted data store");
    }

    auto const output_index =
        PersistedDataStore::output_directory_index(record.output);
    auto const* current = find_output<EventOutputNode, EventOutputState>(
        successor_->event_outputs_,
        successor_->event_output_levels_,
        output_index);
    if (current && current->type != record.type) {
        throw std::invalid_argument(
            "persisted event output chunks disagree on event type");
    }
    if (current && !current->positional_recording) {
        throw std::invalid_argument(
            "persisted event output cannot mix authored Regions with positional recording");
    }

    auto original_record = record;
    auto root = current ? current->chunks
                        : std::shared_ptr<RangeNode<PersistedEventChunk> const>{};
    auto count = current ? current->chunk_count : 0;
    auto coverage = current ? current->coverage : IndexCoverage{};
    auto const original_range = record.range;
    original_range.for_each_signed_order_segment([&](GlobalIndexRange segment) {
        remove_range(root, count, segment);
        auto replacement = slice(record, segment);
        root = put_record(
            root,
            std::make_shared<PersistedEventChunk const>(
                std::move(replacement)));
        ++count;
    });
    coverage.include(original_range);

    auto logical = std::make_shared<std::vector<PersistedEventChunk>>();
    if (current && current->logical_regions) {
        for (auto const& existing : *current->logical_regions) {
            auto remaining = IndexCoverage{existing.range};
            remaining.exclude(original_range);
            for (auto const retained : remaining) {
                logical->push_back(slice(existing, retained));
            }
        }
    }
    logical->push_back(std::move(original_record));
    std::ranges::stable_sort(
        *logical,
        [](PersistedEventChunk const& left,
            PersistedEventChunk const& right) {
            if (left.range.begin == right.range.begin) {
                return left.range.extent > right.range.extent;
            }
            return SignedGlobalIndexOrder{}(
                left.range.begin, right.range.begin);
        });
    auto logical_const = std::shared_ptr<
        std::vector<PersistedEventChunk> const>{std::move(logical)};
    auto region_ranges = make_event_region_ranges(coverage);
    auto timeline = make_event_timeline(logical_const);
    auto accounting = account_event_contents(store_->buffered_pages_, logical_const, root, timeline);

    auto const previous_count = current ? current->chunk_count : 0;
    successor_->event_chunk_count_ =
        successor_->event_chunk_count_ - previous_count + count;
    auto state = std::make_shared<EventOutputState const>(EventOutputState{
        .output = record.output,
        .coverage = std::move(coverage),
        .type = record.type,
        .chunks = std::move(root),
        .logical_regions = std::move(logical_const),
        .region_ranges = std::move(region_ranges),
        .timeline = std::move(timeline),
        .positional_recording = true,
        .chunk_count = count,
        .timeline_accounting = std::move(accounting),
    });
    successor_->event_outputs_ = put_output<EventOutputNode, EventOutputState>(
        successor_->event_outputs_,
        successor_->event_output_levels_,
        output_index,
        std::move(state));
}

void PersistedDataStore::Candidate::replace_sample_regions(
    PersistedOutputHandle output,
    ChannelLayout layout,
    ContinuousValueStorageGeometry geometry,
    IndexCoverage const& invalidated,
    std::span<GlobalIndexRange const> logical_order,
    std::vector<PersistedRegionWrite<PersistedSampleRegion>> writes)
{
    PlaybackPreparationMetrics::Interval timing{PlaybackPreparationPhase::replace_sample_regions};
    if (!successor_) throw std::logic_error("persisted data candidate is empty");
    if (!store_->owns_output(output) || output.kind() != PortKind::sample) {
        throw std::invalid_argument(
            "sample Region batch requires an output handle from its data store");
    }
    validate_sample_representation(layout, geometry);
    auto const incoming_by_region = writes_by_region(logical_order, writes);
    for (auto const& [region_index, region] : writes) {
        if (region.output != output || region.layout != layout || region.geometry != geometry)
            throw std::invalid_argument("sample write has incompatible output storage");
    }
    auto const output_index = PersistedDataStore::output_directory_index(output);
    auto const* current = find_output<SampleOutputNode, SampleOutputState>(
        successor_->sample_outputs_, successor_->sample_output_levels_, output_index);
    if (current && (current->layout != layout || current->geometry != geometry
        || current->positional_recording))
        throw std::invalid_argument("authored sample Regions disagree on representation");
    auto const previous_size = current && current->logical_regions
        ? current->logical_regions->size() : 0;
    std::vector<PersistedSampleRegion> next;
    std::vector<bool> used_old(previous_size, false);
    for (std::size_t region_index = 0; region_index < logical_order.size(); ++region_index) {
        auto const expected = logical_order[region_index];
        if (expected.empty()) throw std::invalid_argument("logical Region is empty");
        PersistedSampleRegion const* prior = nullptr;
        if (current && current->logical_regions) {
            for (std::size_t index = 0; index < previous_size; ++index) {
                if (!used_old[index] && (*current->logical_regions)[index].range == expected) {
                    used_old[index] = true;
                    prior = &(*current->logical_regions)[index];
                    break;
                }
            }
        }
        auto const* incoming = incoming_by_region[region_index];
        auto replaced = invalidated;
        if (incoming) replaced.include(incoming->materialized_coverage());
        next.push_back(replace_tock_region(output, expected, layout, geometry, replaced,
            prior, incoming, store_->buffered_pages_));
    }
    std::shared_ptr<RangeNode<PersistedSampleRegion> const> root{};
    IndexCoverage coverage;
    for (auto const& region : next) {
        auto index_window = [&](PersistedSampleRegion const& window) {
            coverage.include(window.range);
            window.range.for_each_signed_order_segment([&](GlobalIndexRange segment) {
                root = put_record(root, std::make_shared<PersistedSampleRegion const>(
                    segment == window.range ? window : slice(window, segment)));
            });
        };
        if (region.windows) for (auto const& window : *region.windows) index_window(window);
        else index_window(region);
    }
    auto logical = std::make_shared<std::vector<PersistedSampleRegion> const>(
        std::move(next));
    auto const logical_count = logical->size();
    std::size_t physical_count = 0;
    for_each_record(root, [&](PersistedSampleRegion const&) {
        ++physical_count;
    });
    timing.set_work(physical_count);
    auto region_views = make_sample_region_views(logical);
    successor_->sample_region_count_ =
        successor_->sample_region_count_ - previous_size + logical_count;
    auto state = std::make_shared<SampleOutputState const>(SampleOutputState{
        .output = output,
        .coverage = std::move(coverage),
        .logical_coverage = [&] {
            IndexCoverage logical;
            for (auto const range : logical_order) logical.include(range);
            return logical;
        }(),
        .layout = layout,
        .geometry = geometry,
        .regions = std::move(root),
        .logical_regions = std::move(logical),
        .region_views = std::move(region_views),
        .physical_region_count = physical_count,
        .region_count = logical_count,
    });
    successor_->sample_outputs_ = put_output<
        SampleOutputNode, SampleOutputState>(
            successor_->sample_outputs_,
            successor_->sample_output_levels_,
            output_index,
            std::move(state));
}

void PersistedDataStore::Candidate::replace_event_regions(
    PersistedOutputHandle output,
    EventTypeId type,
    IndexCoverage const& invalidated,
    std::span<GlobalIndexRange const> logical_order,
    std::vector<PersistedRegionWrite<PersistedEventChunk>> writes)
{
    if (!successor_) throw std::logic_error("persisted data candidate is empty");
    if (!store_->owns_output(output) || output.kind() != PortKind::event) {
        throw std::invalid_argument(
            "event Region batch requires an output handle from its data store");
    }
    auto const incoming_by_region = writes_by_region(logical_order, writes);
    for (auto const& [region_index, region] : writes) {
        if (region.output != output || region.type != type)
            throw std::invalid_argument("event write has incompatible output storage");
    }
    auto const output_index = PersistedDataStore::output_directory_index(output);
    auto const* current = find_output<EventOutputNode, EventOutputState>(
        successor_->event_outputs_, successor_->event_output_levels_, output_index);
    if (current && (current->type != type || current->positional_recording))
        throw std::invalid_argument("authored event Regions disagree on representation");
    auto const old_count = current && current->logical_regions ? current->logical_regions->size() : 0;
    std::vector<bool> used_old(old_count, false);
    std::vector<PersistedEventChunk> next;
    for (std::size_t region_index = 0; region_index < logical_order.size(); ++region_index) {
        auto const expected = logical_order[region_index];
        if (expected.empty()) throw std::invalid_argument("logical event Region is empty");
        auto const* incoming = incoming_by_region[region_index];
        auto const incoming_coverage = incoming ? incoming->materialized_coverage() : IndexCoverage{};
        auto const replaced = invalidated | incoming_coverage;
        PersistedEventChunk merged{.output = output, .range = expected, .type = type};
        auto valid = incoming_coverage;
        if (current && current->logical_regions) {
            for (std::size_t index = 0; index < old_count; ++index) {
                auto const& prior = (*current->logical_regions)[index];
                if (used_old[index] || prior.range != expected) continue;
                used_old[index] = true;
                valid.include(prior.materialized_coverage() - replaced);
                for (auto const& event : prior.events)
                    if (!replaced.contains(event.time)) merged.events.push_back(event);
                break;
            }
        }
        if (incoming) merged.events.insert(merged.events.end(), incoming->events.begin(), incoming->events.end());
        std::stable_sort(merged.events.begin(), merged.events.end(), [expected](auto const& left, auto const& right) {
            return RangeRelativeIndexOrder{expected.begin}(left.time, right.time);
        });
        merged.prepared_coverage = std::move(valid);
        next.push_back(std::move(merged));
    }

    auto const previous_count = current ? current->chunk_count : 0;
    if (next.empty()) {
        successor_->event_chunk_count_ -= previous_count;
        successor_->event_outputs_ = erase_output_node<EventOutputNode>(
            successor_->event_outputs_,
            successor_->event_output_levels_,
            output_index);
        return;
    }

    std::shared_ptr<RangeNode<PersistedEventChunk> const> root{};
    IndexCoverage coverage;
    std::size_t physical_count = 0;
    for (auto const& region : next) {
        auto const valid = region.materialized_coverage();
        coverage.include(valid);
        for (auto const window : valid) window.for_each_signed_order_segment([&](GlobalIndexRange segment) {
            root = put_record(root, std::make_shared<PersistedEventChunk const>(slice(region, segment)));
            ++physical_count;
        });
    }
    auto logical = std::make_shared<std::vector<PersistedEventChunk> const>(
        std::move(next));
    auto region_ranges = make_event_region_ranges(logical);
    auto timeline = make_event_timeline(logical);
    auto accounting = account_event_contents(store_->buffered_pages_, logical, root, timeline);
    successor_->event_chunk_count_ =
        successor_->event_chunk_count_ - previous_count + physical_count;
    auto state = std::make_shared<EventOutputState const>(EventOutputState{
        .output = output,
        .coverage = std::move(coverage),
        .logical_coverage = [&] {
            IndexCoverage logical;
            for (auto const range : logical_order) logical.include(range);
            return logical;
        }(),
        .type = type,
        .chunks = std::move(root),
        .logical_regions = std::move(logical),
        .region_ranges = std::move(region_ranges),
        .timeline = std::move(timeline),
        .positional_recording = false,
        .chunk_count = physical_count,
        .timeline_accounting = std::move(accounting),
    });
    successor_->event_outputs_ = put_output<EventOutputNode, EventOutputState>(
        successor_->event_outputs_,
        successor_->event_output_levels_,
        output_index,
        std::move(state));
}

void PersistedDataStore::Candidate::erase_sample(
    PersistedOutputHandle output,
    GlobalIndexRange range,
    ChannelLayout layout)
{
    if (!successor_) throw std::logic_error("persisted data candidate is empty");
    if (range.empty()) return;
    if (!store_->owns_output(output) || output.kind() != PortKind::sample) {
        throw std::invalid_argument(
            "sample erasure requires an output handle from its data store");
    }
    auto const output_index =
        PersistedDataStore::output_directory_index(output);
    auto const* current = find_output<SampleOutputNode, SampleOutputState>(
        successor_->sample_outputs_,
        successor_->sample_output_levels_,
        output_index);
    if (!current) return;
    if (current->layout != layout) {
        throw std::invalid_argument(
            "persisted sample erasure has the wrong channel layout");
    }

    if (!current->positional_recording && current->logical_regions) {
        std::vector<GlobalIndexRange> logical_order;
        for (auto const& region : *current->logical_regions) logical_order.push_back(region.range);
        replace_sample_regions(output, layout, current->geometry, IndexCoverage{range}, logical_order, {});
        return;
    }
    if (current->positional_recording) {
        auto coverage = current->coverage;
        coverage.exclude(range);
        if (coverage.empty()) {
            successor_->sample_region_count_ -= current->region_count;
            successor_->sample_outputs_ = erase_output_node<SampleOutputNode>(
                successor_->sample_outputs_,
                successor_->sample_output_levels_,
                output_index);
            return;
        }
        auto pages = current->buffered_pages;
        auto runs = current->recording_runs;
        prune_erased_pages(pages, runs, coverage, range,
            buffered_bytes_per_frame(layout, current->geometry), current->recording_allocator->page_size());
        auto logical = make_recording_regions(
            output,
            coverage,
            layout,
            current->geometry,
            current->recording_allocator,
            runs,
            pages,
            current->logical_regions);
        auto region_views = make_sample_region_views(logical);
        auto const logical_count = logical->size();
        successor_->sample_region_count_ =
            successor_->sample_region_count_ - current->region_count
            + logical_count;
        auto state = std::make_shared<SampleOutputState const>(
            SampleOutputState{
                .output = output,
                .coverage = std::move(coverage),
                .layout = layout,
                .geometry = current->geometry,
                .logical_regions = std::move(logical),
                .region_views = std::move(region_views),
                .recording_allocator = current->recording_allocator,
                .buffered_pages = std::move(pages),
                .recording_runs = std::move(runs),
                .positional_recording = true,
                .region_count = logical_count,
            });
        successor_->sample_outputs_ = put_output<
            SampleOutputNode, SampleOutputState>(
                successor_->sample_outputs_,
                successor_->sample_output_levels_,
                output_index,
                std::move(state));
        return;
    }

    auto root = current->regions;
    auto count = current->physical_region_count;
    range.for_each_signed_order_segment([&](GlobalIndexRange segment) {
        remove_range(root, count, segment);
    });
    if (count == 0) {
        successor_->sample_region_count_ -= current->region_count;
        successor_->sample_outputs_ = erase_output_node<SampleOutputNode>(
            successor_->sample_outputs_,
            successor_->sample_output_levels_,
            output_index);
        return;
    }
    auto coverage = current->coverage;
    coverage.exclude(range);
    auto logical = std::make_shared<std::vector<PersistedSampleRegion>>();
    logical->reserve(count);
    for_each_record(root, [&](PersistedSampleRegion const& retained) {
        logical->push_back(retained);
    });
    auto logical_const = std::shared_ptr<
        std::vector<PersistedSampleRegion> const>{std::move(logical)};
    auto region_views = make_sample_region_views(logical_const);
    auto const logical_count = logical_const->size();
    successor_->sample_region_count_ =
        successor_->sample_region_count_ - current->region_count
        + logical_count;
    auto state = std::make_shared<SampleOutputState const>(SampleOutputState{
        .output = output,
        .coverage = std::move(coverage),
        .layout = layout,
        .geometry = current->geometry,
        .regions = std::move(root),
        .logical_regions = std::move(logical_const),
        .region_views = std::move(region_views),
        .physical_region_count = count,
        .region_count = logical_count,
    });
    successor_->sample_outputs_ = put_output<
        SampleOutputNode, SampleOutputState>(
            successor_->sample_outputs_,
            successor_->sample_output_levels_,
            output_index,
            std::move(state));
}

void PersistedDataStore::Candidate::erase_events(
    PersistedOutputHandle output,
    GlobalIndexRange range,
    EventTypeId type)
{
    if (!successor_) throw std::logic_error("persisted data candidate is empty");
    if (range.empty()) return;
    if (!store_->owns_output(output) || output.kind() != PortKind::event) {
        throw std::invalid_argument(
            "event erasure requires an output handle from its data store");
    }
    auto const output_index =
        PersistedDataStore::output_directory_index(output);
    auto const* current = find_output<EventOutputNode, EventOutputState>(
        successor_->event_outputs_,
        successor_->event_output_levels_,
        output_index);
    if (!current) return;
    if (current->type != type) {
        throw std::invalid_argument(
            "persisted event erasure has the wrong event type");
    }

    if (!current->positional_recording && current->logical_regions) {
        std::vector<GlobalIndexRange> logical_order;
        for (auto const& region : *current->logical_regions) logical_order.push_back(region.range);
        replace_event_regions(output, type, IndexCoverage{range}, logical_order, {});
        return;
    }
    auto root = current->chunks;
    auto count = current->chunk_count;
    range.for_each_signed_order_segment([&](GlobalIndexRange segment) {
        remove_range(root, count, segment);
    });
    successor_->event_chunk_count_ =
        successor_->event_chunk_count_ - current->chunk_count + count;
    if (count == 0) {
        successor_->event_outputs_ = erase_output_node<EventOutputNode>(
            successor_->event_outputs_,
            successor_->event_output_levels_,
            output_index);
        return;
    }
    auto coverage = current->coverage;
    coverage.exclude(range);
    auto logical = std::make_shared<std::vector<PersistedEventChunk>>();
    if (current->logical_regions) {
        for (auto const& existing : *current->logical_regions) {
            auto remaining = IndexCoverage{existing.range};
            remaining.exclude(range);
            for (auto const retained : remaining) {
                logical->push_back(slice(existing, retained));
            }
        }
    } else {
        for_each_record(root, [&](PersistedEventChunk const& retained) {
            logical->push_back(retained);
        });
    }
    auto logical_const = std::shared_ptr<
        std::vector<PersistedEventChunk> const>{std::move(logical)};
    auto region_ranges = current->positional_recording
        ? make_event_region_ranges(coverage)
        : make_event_region_ranges(logical_const);
    auto timeline = make_event_timeline(logical_const);
    auto accounting = account_event_contents(store_->buffered_pages_, logical_const, root, timeline);
    auto state = std::make_shared<EventOutputState const>(EventOutputState{
        .output = output,
        .coverage = std::move(coverage),
        .type = type,
        .chunks = std::move(root),
        .logical_regions = std::move(logical_const),
        .region_ranges = std::move(region_ranges),
        .timeline = std::move(timeline),
        .positional_recording = current->positional_recording,
        .chunk_count = count,
        .timeline_accounting = std::move(accounting),
    });
    successor_->event_outputs_ = put_output<EventOutputNode, EventOutputState>(
        successor_->event_outputs_,
        successor_->event_output_levels_,
        output_index,
        std::move(state));
}

void PersistedDataStore::Candidate::erase_output(
    PersistedOutputHandle output)
{
    if (!successor_) throw std::logic_error("persisted data candidate is empty");
    if (!store_->owns_output(output)) {
        throw std::invalid_argument(
            "persisted output erasure requires a handle from its data store");
    }
    erased_outputs_.push_back(output);
    auto const output_index =
        PersistedDataStore::output_directory_index(output);
    if (output.kind() == PortKind::sample) {
        auto const* current = find_output<SampleOutputNode, SampleOutputState>(
            successor_->sample_outputs_,
            successor_->sample_output_levels_,
            output_index);
        if (!current) return;
        successor_->sample_region_count_ -= current->region_count;
        successor_->sample_outputs_ = erase_output_node<SampleOutputNode>(
            successor_->sample_outputs_,
            successor_->sample_output_levels_,
            output_index);
    } else {
        auto const* current = find_output<EventOutputNode, EventOutputState>(
            successor_->event_outputs_,
            successor_->event_output_levels_,
            output_index);
        if (!current) return;
        successor_->event_chunk_count_ -= current->chunk_count;
        successor_->event_outputs_ = erase_output_node<EventOutputNode>(
            successor_->event_outputs_,
            successor_->event_output_levels_,
            output_index);
    }
}

PersistedDataStore::ReaderPin::ReaderPin(
    ReaderSlotState& slot,
    std::atomic<Snapshot const*> const& published) noexcept
    : slot_(&slot)
{
    assert(slot.pinned.load(std::memory_order_seq_cst) == nullptr);
    slot.acquiring.store(true, std::memory_order_seq_cst);
    snapshot_ = published.load(std::memory_order_seq_cst);
    slot.pinned.store(snapshot_, std::memory_order_seq_cst);
    slot.acquiring.store(false, std::memory_order_seq_cst);
}

void PersistedDataStore::ReaderPin::reset() noexcept
{
    if (slot_) slot_->pinned.store(nullptr, std::memory_order_seq_cst);
    slot_ = nullptr;
    snapshot_ = nullptr;
    owned_snapshot_.reset();
}

PersistedDataStore::ReaderPin::~ReaderPin()
{
    reset();
}

PersistedDataStore::ReaderPin::ReaderPin(ReaderPin&& other) noexcept
    : slot_(std::exchange(other.slot_, nullptr))
    , snapshot_(std::exchange(other.snapshot_, nullptr))
    , owned_snapshot_(std::move(other.owned_snapshot_))
{}

PersistedDataStore::ReaderPin& PersistedDataStore::ReaderPin::operator=(
    ReaderPin&& other) noexcept
{
    if (this == &other) return *this;
    reset();
    slot_ = std::exchange(other.slot_, nullptr);
    snapshot_ = std::exchange(other.snapshot_, nullptr);
    owned_snapshot_ = std::move(other.owned_snapshot_);
    return *this;
}

PersistedDataStore::Snapshot const&
PersistedDataStore::ReaderPin::snapshot() const noexcept
{
    assert(snapshot_ != nullptr);
    return *snapshot_;
}

PersistedDataStore::ReaderSlot::ReaderSlot(
    PersistedDataStore& store,
    ReaderSlotState& state) noexcept
    : store_(&store)
    , state_(&state)
{}

void PersistedDataStore::ReaderSlot::reset() noexcept
{
    if (!state_) return;
    store_->unregister_reader(*state_);
    store_ = nullptr;
    state_ = nullptr;
}

PersistedDataStore::ReaderSlot::~ReaderSlot()
{
    reset();
}

PersistedDataStore::ReaderSlot::ReaderSlot(ReaderSlot&& other) noexcept
    : store_(std::exchange(other.store_, nullptr))
    , state_(std::exchange(other.state_, nullptr))
{}

PersistedDataStore::ReaderSlot& PersistedDataStore::ReaderSlot::operator=(
    ReaderSlot&& other) noexcept
{
    if (this == &other) return *this;
    reset();
    store_ = std::exchange(other.store_, nullptr);
    state_ = std::exchange(other.state_, nullptr);
    return *this;
}

PersistedDataStore::ReaderPin PersistedDataStore::ReaderSlot::pin() noexcept
{
    assert(store_ != nullptr && state_ != nullptr);
    return ReaderPin{*state_, store_->published_};
}

PersistedDataStore::PersistedDataStore(BufferedStorageLimits limits)
    : buffered_pages_(std::make_shared<BufferedPageAllocator>(limits))
    , output_owner_token_(allocate_persisted_output_owner())
    , published_owner_([this] {
        auto snapshot = std::make_unique<Snapshot>();
        snapshot->output_owner_token_ = output_owner_token_;
        snapshot->logical_identity_ = std::make_shared<std::byte>();
        return snapshot;
    }())
    , published_(published_owner_.get())
{}

PersistedDataStore::~PersistedDataStore()
{
    assert(reader_slots_.empty());
}

BufferedStorageUsage PersistedDataStore::buffered_storage_usage() const
{
    auto usage = buffered_pages_->usage();
    usage.cache_evictions = cache_evictions_.load(std::memory_order_relaxed);
    return usage;
}

std::shared_ptr<void const> PersistedDataStore::prepare_resident(std::span<BufferedResidentRange const> ranges)
{
    return buffered_pages_->prepare_resident(ranges);
}

PersistedDataStore::Candidate PersistedDataStore::begin_candidate(
    std::uint64_t target_semantic_version)
{
    auto const* base = published_.load(std::memory_order_seq_cst);
    if (target_semantic_version < base->version_.semantic) {
        throw std::invalid_argument(
            "persisted data candidate semantic version cannot move backwards");
    }
    auto revision = last_candidate_revision_.load(std::memory_order_relaxed);
    for (;;) {
        if (revision == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("persisted data revision is exhausted");
        }
        if (last_candidate_revision_.compare_exchange_weak(
                revision, revision + 1, std::memory_order_relaxed)) break;
    }

    auto successor = std::make_unique<Snapshot>();
    successor->logical_identity_ = std::make_shared<std::byte>();
    successor->version_ = {
        .semantic = target_semantic_version,
        .revision = revision + 1,
    };
    successor->output_owner_token_ = base->output_owner_token_;
    successor->sample_outputs_ = base->sample_outputs_;
    successor->sample_output_levels_ = base->sample_output_levels_;
    successor->event_outputs_ = base->event_outputs_;
    successor->event_output_levels_ = base->event_output_levels_;
    successor->sample_region_count_ = base->sample_region_count_;
    successor->event_chunk_count_ = base->event_chunk_count_;
    return Candidate{*this, *base, std::move(successor)};
}

PersistedDataPublishResult PersistedDataStore::publish(Candidate&& candidate)
{
    if (candidate.store_ != this || !candidate.successor_) {
        throw std::invalid_argument(
            "persisted data candidate does not belong to this store");
    }
    auto const* current = published_.load(std::memory_order_seq_cst);
    if (current->logical_identity_ != candidate.base_identity_
        || current->version() != candidate.base_version_) {
        candidate = {};
        return PersistedDataPublishResult::stale_base;
    }

    retired_.push_back(std::move(published_owner_));
    published_owner_ = std::move(candidate.successor_);
    published_.store(published_owner_.get(), std::memory_order_seq_cst);
    published_revision_.store(published_owner_->version_.revision, std::memory_order_release);
    for (auto output : candidate.erased_outputs_) {
        auto index = output_directory_index(output);
        bool const present = output.kind() == PortKind::sample
            ? find_output<SampleOutputNode, SampleOutputState>(published_owner_->sample_outputs_,
                published_owner_->sample_output_levels_, index) != nullptr
            : find_output<EventOutputNode, EventOutputState>(published_owner_->event_outputs_,
                published_owner_->event_output_levels_, index) != nullptr;
        if (!present) cache_policy_.erase(output);
    }
    candidate.store_ = nullptr;
    candidate.base_identity_.reset();
    return PersistedDataPublishResult::published;
}

PersistedDataStore::ReaderSlot PersistedDataStore::register_reader()
{
    auto state = std::make_unique<ReaderSlotState>();
    auto* pointer = state.get();
    std::lock_guard lock{reader_slots_mutex_};
    reader_slots_.push_back(std::move(state));
    return ReaderSlot{*this, *pointer};
}

void PersistedDataStore::unregister_reader(ReaderSlotState& state) noexcept
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

std::size_t PersistedDataStore::reclaim_retired()
{
    std::lock_guard lock{reader_slots_mutex_};
    if (std::ranges::any_of(reader_slots_, [](auto const& slot) {
            return slot->acquiring.load(std::memory_order_seq_cst);
        })) {
        return 0;
    }
    auto const before = retired_.size();
    std::erase_if(retired_, [&](auto const& snapshot) {
        return std::ranges::none_of(reader_slots_, [&](auto const& slot) {
            return slot->pinned.load(std::memory_order_seq_cst)
                == snapshot.get();
        });
    });
    return before - retired_.size();
}

} // namespace iv
