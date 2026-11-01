#include <intravenous/runtime/persisted_data_store.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <limits>
#include <iterator>
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

struct RecordingPageKey {
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    auto operator<=>(RecordingPageKey const&) const = default;
};

struct RecordingPage {
    std::uint64_t file_offset = 0;
};

struct RecordingPageNode {
    RecordingPageKey key{};
    std::shared_ptr<RecordingPage const> page{};
    std::shared_ptr<RecordingPageNode const> left{};
    std::shared_ptr<RecordingPageNode const> right{};
    std::uint32_t height = 1;
};

struct RecordingPageRun {
    RecordingPageKey first{};
    std::size_t page_count = 0;
    std::uint64_t file_offset = 0;
};

struct RecordingPageRunNode {
    RecordingPageRun run{};
    std::shared_ptr<RecordingPageRunNode const> left{};
    std::shared_ptr<RecordingPageRunNode const> right{};
    std::uint32_t height = 1;
};

class RecordingPageAllocator {
    int file_ = -1;
    std::size_t page_size_ = 0;
    std::uint64_t next_file_offset_ = 0;

public:
    RecordingPageAllocator();
    ~RecordingPageAllocator();

    RecordingPageAllocator(RecordingPageAllocator const&) = delete;
    RecordingPageAllocator& operator=(RecordingPageAllocator const&) = delete;

    [[nodiscard]] int file() const noexcept { return file_; }
    [[nodiscard]] std::size_t page_size() const noexcept { return page_size_; }
    void read(RecordingPage const& page, std::span<std::byte> target) const;
    [[nodiscard]] std::shared_ptr<RecordingPage const> allocate(
        std::span<std::byte const> contents);
};

struct SampleOutputState {
    PersistedOutputHandle output{};
    IndexCoverage coverage{};
    ChannelLayout layout{};
    ContinuousValueStorageGeometry geometry{};
    std::shared_ptr<RangeNode<PersistedSampleRegion> const> regions{};
    std::shared_ptr<std::vector<PersistedSampleRegion> const> logical_regions{};
    std::shared_ptr<std::vector<ContinuousRegionStorageView> const> region_views{};
    std::shared_ptr<RecordingPageAllocator> recording_allocator{};
    std::shared_ptr<RecordingPageNode const> recording_pages{};
    std::shared_ptr<RecordingPageRunNode const> recording_runs{};
    bool positional_recording = false;
    std::size_t physical_region_count = 0;
    std::size_t region_count = 0;
};

struct EventOutputState {
    PersistedOutputHandle output{};
    IndexCoverage coverage{};
    EventTypeId type = EventTypeId::empty;
    std::shared_ptr<RangeNode<PersistedEventChunk> const> chunks{};
    std::shared_ptr<std::vector<PersistedEventChunk> const> logical_regions{};
    std::shared_ptr<std::vector<GlobalIndexRange> const> region_ranges{};
    std::shared_ptr<std::vector<TimedEvent> const> timeline{};
    bool positional_recording = false;
    std::size_t chunk_count = 0;
};

template<typename State>
struct OutputDirectoryNode {
    using Node = OutputDirectoryNode<State>;
    using Children = std::array<std::shared_ptr<Node const>, 16>;
    using Values = std::array<std::shared_ptr<State const>, 16>;

    std::variant<Children, Values> entries{Values{}};
};

} // namespace persisted_data_store_detail

persisted_data_store_detail::RecordingPageAllocator::RecordingPageAllocator()
{
#if defined(__linux__)
    auto const page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        throw std::system_error(
            errno, std::generic_category(), "cannot query the Linux page size");
    }
    page_size_ = static_cast<std::size_t>(page_size);
    file_ = ::memfd_create("iv-persisted-recording", MFD_CLOEXEC);
    if (file_ < 0) {
        throw std::system_error(
            errno, std::generic_category(), "cannot create recording page storage");
    }
#else
    throw std::runtime_error(
        "persisted Tick continuous recording requires Linux virtual memory");
#endif
}

persisted_data_store_detail::RecordingPageAllocator::~RecordingPageAllocator()
{
#if defined(__linux__)
    if (file_ >= 0) ::close(file_);
#endif
}

void persisted_data_store_detail::RecordingPageAllocator::read(
    RecordingPage const& page,
    std::span<std::byte> target) const
{
    if (target.size() != page_size_) {
        throw std::invalid_argument("recording page read has the wrong size");
    }
#if defined(__linux__)
    std::size_t read_bytes = 0;
    while (read_bytes != target.size()) {
        auto const result = ::pread(
            file_,
            target.data() + read_bytes,
            target.size() - read_bytes,
            static_cast<off_t>(page.file_offset + read_bytes));
        if (result < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(
                errno, std::generic_category(), "cannot read a recording page");
        }
        if (result == 0) {
            throw std::runtime_error("recording page storage ended unexpectedly");
        }
        read_bytes += static_cast<std::size_t>(result);
    }
#else
    (void)page;
#endif
}

std::shared_ptr<persisted_data_store_detail::RecordingPage const>
persisted_data_store_detail::RecordingPageAllocator::allocate(
    std::span<std::byte const> contents)
{
    if (contents.size() != page_size_) {
        throw std::invalid_argument("recording page write has the wrong size");
    }
    if (next_file_offset_
        > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())
            - page_size_) {
        throw std::length_error("recording page storage is exhausted");
    }
    auto const offset = next_file_offset_;
    auto const new_file_size = next_file_offset_ + page_size_;
#if defined(__linux__)
    if (::ftruncate(file_, static_cast<off_t>(new_file_size)) != 0) {
        throw std::system_error(
            errno, std::generic_category(), "cannot extend recording page storage");
    }
    std::size_t written = 0;
    while (written != contents.size()) {
        auto const result = ::pwrite(
            file_,
            contents.data() + written,
            contents.size() - written,
            static_cast<off_t>(offset + written));
        if (result < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(
                errno, std::generic_category(), "cannot write a recording page");
        }
        if (result == 0) {
            throw std::runtime_error("recording page write made no progress");
        }
        written += static_cast<std::size_t>(result);
    }
#endif
    next_file_offset_ = new_file_size;
    return std::make_shared<RecordingPage const>(RecordingPage{
        .file_offset = offset,
    });
}

namespace {

using persisted_data_store_detail::EventOutputNode;
using persisted_data_store_detail::EventOutputState;
using persisted_data_store_detail::RangeNode;
using persisted_data_store_detail::RecordingPage;
using persisted_data_store_detail::RecordingPageAllocator;
using persisted_data_store_detail::RecordingPageKey;
using persisted_data_store_detail::RecordingPageNode;
using persisted_data_store_detail::RecordingPageRun;
using persisted_data_store_detail::RecordingPageRunNode;
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
    case EventTypeId::midi:
        return std::holds_alternative<MidiEvent>(event);
    case EventTypeId::count:
        break;
    }
    return false;
}

void validate(PersistedSampleRegion const& region)
{
    if (!region.output.valid()
        || persisted_output_kind(region.output) != PortKind::sample) {
        throw std::invalid_argument(
            "sample region requires a sample output identity");
    }
    if (!is_valid_channel_type(region.layout.channel_type)
        || !is_valid_sample_stream_layout(region.layout.sample_layout)) {
        throw std::invalid_argument(
            "sample region has an invalid channel layout");
    }
    if (!is_registered_continuous_value_type(region.geometry.value_type)
        || region.geometry.value_size_bytes == 0
        || region.geometry.element_alignment == 0
        || !std::has_single_bit(region.geometry.element_alignment)) {
        throw std::invalid_argument(
            "sample region has invalid continuous-value geometry");
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
    GlobalIndex::extent_type previous = 0;
    bool first = true;
    for (auto const& event : chunk.events) {
        auto const offset = chunk.range.offset_of(event.time);
        if (!offset || !event_matches_type(chunk.type, event.value)) {
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

[[nodiscard]] PersistedSampleRegion slice(
    PersistedSampleRegion const& source,
    GlobalIndexRange range)
{
    assert(source.range.contains(range) && !range.empty());
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

[[nodiscard]] std::shared_ptr<RecordingPageNode const> make_recording_page_node(
    RecordingPageKey key,
    std::shared_ptr<RecordingPage const> page,
    std::shared_ptr<RecordingPageNode const> left = {},
    std::shared_ptr<RecordingPageNode const> right = {})
{
    auto const height = 1 + std::max(node_height(left), node_height(right));
    return std::make_shared<RecordingPageNode const>(RecordingPageNode{
        .key = key,
        .page = std::move(page),
        .left = std::move(left),
        .right = std::move(right),
        .height = height,
    });
}

[[nodiscard]] std::shared_ptr<RecordingPageNode const> balance_recording_page_node(
    std::shared_ptr<RecordingPageNode const> node)
{
    auto const balance = static_cast<int>(node_height(node->left))
        - static_cast<int>(node_height(node->right));
    if (balance > 1) {
        auto left = node->left;
        if (node_height(left->left) < node_height(left->right)) {
            auto const pivot = left->right;
            left = make_recording_page_node(
                left->key, left->page, left->left, pivot->left);
            left = make_recording_page_node(
                pivot->key, pivot->page, std::move(left), pivot->right);
        }
        auto const new_right = make_recording_page_node(
            node->key, node->page, left->right, node->right);
        return make_recording_page_node(
            left->key, left->page, left->left, std::move(new_right));
    }
    if (balance < -1) {
        auto right = node->right;
        if (node_height(right->right) < node_height(right->left)) {
            auto const pivot = right->left;
            right = make_recording_page_node(
                right->key, right->page, pivot->right, right->right);
            right = make_recording_page_node(
                pivot->key, pivot->page, pivot->left, std::move(right));
        }
        auto const new_left = make_recording_page_node(
            node->key, node->page, node->left, right->left);
        return make_recording_page_node(
            right->key, right->page, std::move(new_left), right->right);
    }
    return node;
}

[[nodiscard]] std::shared_ptr<RecordingPageNode const> put_recording_page(
    std::shared_ptr<RecordingPageNode const> const& root,
    RecordingPageKey key,
    std::shared_ptr<RecordingPage const> page)
{
    if (!root) {
        return make_recording_page_node(key, std::move(page));
    }
    if (key < root->key) {
        return balance_recording_page_node(make_recording_page_node(
            root->key,
            root->page,
            put_recording_page(root->left, key, std::move(page)),
            root->right));
    }
    if (root->key < key) {
        return balance_recording_page_node(make_recording_page_node(
            root->key,
            root->page,
            root->left,
            put_recording_page(root->right, key, std::move(page))));
    }
    return make_recording_page_node(
        key, std::move(page), root->left, root->right);
}

[[nodiscard]] RecordingPage const* find_recording_page(
    std::shared_ptr<RecordingPageNode const> const& root,
    RecordingPageKey key) noexcept
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

using RecordingByteAddress = unsigned __int128;

[[nodiscard]] RecordingPageKey recording_page_key(
    RecordingByteAddress address,
    std::size_t page_size) noexcept
{
    auto const page = address / page_size;
    return {
        .high = static_cast<std::uint64_t>(page >> 64),
        .low = static_cast<std::uint64_t>(page),
    };
}

[[nodiscard]] RecordingPageKey add_recording_pages(
    RecordingPageKey key,
    std::size_t count) noexcept
{
    auto const previous = key.low;
    key.low += static_cast<std::uint64_t>(count);
    if (key.low < previous) ++key.high;
    return key;
}

using RecordingPageNumber = unsigned __int128;

[[nodiscard]] RecordingPageNumber recording_page_number(
    RecordingPageKey key) noexcept
{
    return (static_cast<RecordingPageNumber>(key.high) << 64) | key.low;
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

[[nodiscard]] std::shared_ptr<RecordingPageRunNode const>
make_recording_page_run_node(
    RecordingPageRun run,
    std::shared_ptr<RecordingPageRunNode const> left = {},
    std::shared_ptr<RecordingPageRunNode const> right = {})
{
    auto const height = 1 + std::max(node_height(left), node_height(right));
    return std::make_shared<RecordingPageRunNode const>(RecordingPageRunNode{
        .run = run,
        .left = std::move(left),
        .right = std::move(right),
        .height = height,
    });
}

[[nodiscard]] std::shared_ptr<RecordingPageRunNode const>
balance_recording_page_run_node(
    std::shared_ptr<RecordingPageRunNode const> node)
{
    auto const balance = static_cast<int>(node_height(node->left))
        - static_cast<int>(node_height(node->right));
    if (balance > 1) {
        auto left = node->left;
        if (node_height(left->left) < node_height(left->right)) {
            auto const pivot = left->right;
            left = make_recording_page_run_node(
                left->run, left->left, pivot->left);
            left = make_recording_page_run_node(
                pivot->run, std::move(left), pivot->right);
        }
        auto const new_right = make_recording_page_run_node(
            node->run, left->right, node->right);
        return make_recording_page_run_node(
            left->run, left->left, std::move(new_right));
    }
    if (balance < -1) {
        auto right = node->right;
        if (node_height(right->right) < node_height(right->left)) {
            auto const pivot = right->left;
            right = make_recording_page_run_node(
                right->run, pivot->right, right->right);
            right = make_recording_page_run_node(
                pivot->run, pivot->left, std::move(right));
        }
        auto const new_left = make_recording_page_run_node(
            node->run, node->left, right->left);
        return make_recording_page_run_node(
            right->run, std::move(new_left), right->right);
    }
    return node;
}

[[nodiscard]] std::shared_ptr<RecordingPageRunNode const>
put_recording_page_run(
    std::shared_ptr<RecordingPageRunNode const> const& root,
    RecordingPageRun run)
{
    if (!root) return make_recording_page_run_node(run);
    if (run.first < root->run.first) {
        return balance_recording_page_run_node(make_recording_page_run_node(
            root->run,
            put_recording_page_run(root->left, run),
            root->right));
    }
    if (root->run.first < run.first) {
        return balance_recording_page_run_node(make_recording_page_run_node(
            root->run,
            root->left,
            put_recording_page_run(root->right, run)));
    }
    return make_recording_page_run_node(run, root->left, root->right);
}

[[nodiscard]] std::shared_ptr<RecordingPageRunNode const>
erase_recording_page_run(
    std::shared_ptr<RecordingPageRunNode const> const& root,
    RecordingPageKey first)
{
    if (!root) return {};
    if (first < root->run.first) {
        return balance_recording_page_run_node(make_recording_page_run_node(
            root->run,
            erase_recording_page_run(root->left, first),
            root->right));
    }
    if (root->run.first < first) {
        return balance_recording_page_run_node(make_recording_page_run_node(
            root->run,
            root->left,
            erase_recording_page_run(root->right, first)));
    }
    if (!root->left) return root->right;
    if (!root->right) return root->left;
    auto successor = root->right;
    while (successor->left) successor = successor->left;
    return balance_recording_page_run_node(make_recording_page_run_node(
        successor->run,
        root->left,
        erase_recording_page_run(root->right, successor->run.first)));
}

[[nodiscard]] RecordingPageRun const* find_recording_page_run(
    std::shared_ptr<RecordingPageRunNode const> const& root,
    RecordingPageKey page) noexcept
{
    auto const page_number = recording_page_number(page);
    auto const* node = root.get();
    RecordingPageRun const* candidate = nullptr;
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
        - recording_page_number(candidate->first);
    return offset < candidate->page_count ? candidate : nullptr;
}

[[nodiscard]] RecordingPageRun const* find_recording_page_run_before(
    std::shared_ptr<RecordingPageRunNode const> const& root,
    RecordingPageKey first) noexcept
{
    auto const* node = root.get();
    RecordingPageRun const* result = nullptr;
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

[[nodiscard]] RecordingPageRun const* find_recording_page_run_after(
    std::shared_ptr<RecordingPageRunNode const> const& root,
    RecordingPageKey first) noexcept
{
    auto const* node = root.get();
    RecordingPageRun const* result = nullptr;
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

[[nodiscard]] bool recording_page_runs_are_contiguous(
    RecordingPageRun const& left,
    RecordingPageRun const& right,
    std::size_t page_size) noexcept
{
    return add_recording_pages(left.first, left.page_count) == right.first
        && left.file_offset + left.page_count * page_size
            == right.file_offset;
}

[[nodiscard]] std::shared_ptr<RecordingPageRunNode const>
put_coalesced_recording_page_run(
    std::shared_ptr<RecordingPageRunNode const> root,
    RecordingPageRun run,
    std::size_t page_size)
{
    if (auto const* previous = find_recording_page_run_before(
            root, run.first);
        previous && recording_page_runs_are_contiguous(
            *previous, run, page_size)) {
        auto const first = previous->first;
        auto const file_offset = previous->file_offset;
        auto const page_count = previous->page_count;
        root = erase_recording_page_run(root, first);
        run.first = first;
        run.file_offset = file_offset;
        run.page_count = checked_page_count_sum(
            page_count, run.page_count);
    }
    if (auto const* next = find_recording_page_run_after(root, run.first);
        next && recording_page_runs_are_contiguous(run, *next, page_size)) {
        auto const first = next->first;
        auto const page_count = next->page_count;
        root = erase_recording_page_run(root, first);
        run.page_count = checked_page_count_sum(
            run.page_count, page_count);
    }
    return put_recording_page_run(root, run);
}

[[nodiscard]] std::shared_ptr<RecordingPageRunNode const>
replace_recording_page_run(
    std::shared_ptr<RecordingPageRunNode const> root,
    RecordingPageKey page,
    std::uint64_t file_offset,
    std::size_t page_size)
{
    if (auto const* previous = find_recording_page_run(root, page)) {
        auto const old = *previous;
        root = erase_recording_page_run(root, old.first);
        auto const prefix_number = recording_page_number(page)
            - recording_page_number(old.first);
        auto const prefix = static_cast<std::size_t>(prefix_number);
        assert(prefix < old.page_count);
        if (prefix != 0) {
            root = put_recording_page_run(root, RecordingPageRun{
                .first = old.first,
                .page_count = prefix,
                .file_offset = old.file_offset,
            });
        }
        auto const suffix = old.page_count - prefix - 1;
        if (suffix != 0) {
            root = put_recording_page_run(root, RecordingPageRun{
                .first = add_recording_pages(page, 1),
                .page_count = suffix,
                .file_offset = old.file_offset
                    + (prefix + 1) * page_size,
            });
        }
    }
    return put_coalesced_recording_page_run(
        std::move(root),
        RecordingPageRun{
            .first = page,
            .page_count = 1,
            .file_offset = file_offset,
        },
        page_size);
}

[[nodiscard]] RecordingByteAddress recording_byte_address(
    GlobalIndex index,
    std::size_t bytes_per_frame) noexcept
{
    return static_cast<RecordingByteAddress>(
        SignedGlobalIndexOrder::key(index)) * bytes_per_frame;
}

[[nodiscard]] std::size_t recording_bytes_per_frame(
    ChannelLayout layout,
    ContinuousValueStorageGeometry geometry)
{
    return checked_product(
        channel_count(layout), geometry.value_size_bytes);
}

void write_recording_region(
    std::shared_ptr<RecordingPageNode const>& root,
    std::shared_ptr<RecordingPageRunNode const>& runs,
    RecordingPageAllocator& allocator,
    PersistedSampleRegion const& record)
{
    assert(record.layout.sample_layout == SampleStreamLayout::interleaved);
    auto const bytes_per_frame = recording_bytes_per_frame(
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
        auto address = recording_byte_address(
            segment.begin, bytes_per_frame);
        while (remaining != 0) {
            auto const key = recording_page_key(address, page_size);
            auto const in_page = static_cast<std::size_t>(address % page_size);
            auto const count = std::min(remaining, page_size - in_page);
            std::vector<std::byte> contents(page_size);
            if (auto const* previous = find_recording_page(root, key)) {
                allocator.read(*previous, contents);
            }
            std::memcpy(contents.data() + in_page, source, count);
            auto page = allocator.allocate(contents);
            runs = replace_recording_page_run(
                std::move(runs),
                key,
                page->file_offset,
                page_size);
            root = put_recording_page(root, key, std::move(page));
            source += count;
            remaining -= count;
            address += count;
        }
    });
}

class RecordingRegionMapping {
    void* mapping_ = nullptr;
    std::size_t mapping_size_ = 0;

public:
    std::shared_ptr<RecordingPageAllocator> allocator{};
    std::shared_ptr<RecordingPageRunNode const> runs{};

    RecordingRegionMapping(
        void* mapping,
        std::size_t mapping_size,
        std::shared_ptr<RecordingPageAllocator> allocator_value,
        std::shared_ptr<RecordingPageRunNode const> runs_value) noexcept
        : mapping_(mapping)
        , mapping_size_(mapping_size)
        , allocator(std::move(allocator_value))
        , runs(std::move(runs_value))
    {}

    ~RecordingRegionMapping()
    {
#if defined(__linux__)
        if (mapping_) ::munmap(mapping_, mapping_size_);
#endif
    }

    RecordingRegionMapping(RecordingRegionMapping const&) = delete;
    RecordingRegionMapping& operator=(RecordingRegionMapping const&) = delete;
};

[[nodiscard]] PersistedSampleRegion map_recording_region(
    PersistedOutputHandle output,
    GlobalIndexRange range,
    ChannelLayout layout,
    ContinuousValueStorageGeometry geometry,
    std::shared_ptr<RecordingPageAllocator> const& allocator,
    std::shared_ptr<RecordingPageRunNode const> const& runs)
{
    assert(layout.sample_layout == SampleStreamLayout::interleaved);
    auto const bytes_per_frame = recording_bytes_per_frame(layout, geometry);
    auto const bytes = checked_product(
        checked_frame_count(range), bytes_per_frame);
    auto const page_size = allocator->page_size();
    auto const address = recording_byte_address(range.begin, bytes_per_frame);
    auto const in_page = static_cast<std::size_t>(address % page_size);
    if (bytes > std::numeric_limits<std::size_t>::max() - in_page) {
        throw std::length_error("recording Region mapping is too large");
    }
    auto const occupied = in_page + bytes;
    auto const page_count = occupied / page_size
        + static_cast<std::size_t>(occupied % page_size != 0);
    auto const mapping_size = checked_product(page_count, page_size);
#if defined(__linux__)
    auto* mapping = ::mmap(
        nullptr,
        mapping_size,
        PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
    if (mapping == MAP_FAILED) {
        throw std::system_error(
            errno, std::generic_category(), "cannot reserve a recording Region mapping");
    }
    auto const first_key = recording_page_key(address, page_size);
    std::size_t mapped_pages = 0;
    try {
        while (mapped_pages != page_count) {
            auto const key = add_recording_pages(first_key, mapped_pages);
            auto const* run = find_recording_page_run(runs, key);
            if (!run) {
                throw std::logic_error(
                    "recording coverage references an unallocated page");
            }
            auto const run_offset_number = recording_page_number(key)
                - recording_page_number(run->first);
            auto const run_offset = static_cast<std::size_t>(
                run_offset_number);
            assert(run_offset < run->page_count);
            auto const run_pages = std::min(
                page_count - mapped_pages,
                run->page_count - run_offset);
            auto const run_size = checked_product(run_pages, page_size);
            auto* const target = static_cast<std::byte*>(mapping)
                + mapped_pages * page_size;
            if (::mmap(
                    target,
                    run_size,
                    PROT_READ,
                    MAP_SHARED | MAP_FIXED,
                    allocator->file(),
                    static_cast<off_t>(run->file_offset
                        + run_offset * page_size)) == MAP_FAILED) {
                throw std::system_error(
                    errno, std::generic_category(), "cannot map recording Region pages");
            }
            mapped_pages += run_pages;
        }
    } catch (...) {
        ::munmap(mapping, mapping_size);
        throw;
    }
    auto owner = std::make_shared<RecordingRegionMapping const>(
        mapping, mapping_size, allocator, runs);
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
    };
#else
    (void)output;
    (void)runs;
    throw std::runtime_error(
        "persisted Tick continuous recording requires Linux virtual memory");
#endif
}

[[nodiscard]] std::shared_ptr<std::vector<PersistedSampleRegion> const>
make_recording_regions(
    PersistedOutputHandle output,
    IndexCoverage const& coverage,
    ChannelLayout layout,
    ContinuousValueStorageGeometry geometry,
    std::shared_ptr<RecordingPageAllocator> const& allocator,
    std::shared_ptr<RecordingPageRunNode const> const& runs)
{
    auto regions = std::make_shared<std::vector<PersistedSampleRegion>>();
    regions->reserve(coverage.size());
    for (auto const range : coverage) {
        regions->push_back(map_recording_region(
            output, range, layout, geometry, allocator, runs));
    }
    return std::shared_ptr<std::vector<PersistedSampleRegion> const>{
        std::move(regions)};
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
    if (planned.storage != graph_jit::PortStorageKind::persisted_data
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
                return region.range.contains(index);
            });
        return found == selected->logical_regions->end()
            ? nullptr
            : std::addressof(*found);
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
                return region.range.contains(index);
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
    for (auto const& record : *selected->logical_regions) {
        range.for_each_signed_order_segment([&](GlobalIndexRange requested) {
            record.range.for_each_signed_order_segment(
                [&](GlobalIndexRange stored) {
                    auto const part = intersection(stored, requested);
                    if (!part.empty()) visitor(visitor_data, record, part);
                });
        });
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
        for (auto const& event : (*selected->logical_regions)[region_index].events) {
            if (requested.contains(event.time)) visitor(visitor_data, event);
        }
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
    , base_(&base)
    , base_version_(base.version())
    , successor_(std::move(successor))
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

    auto allocator = current && current->recording_allocator
        ? current->recording_allocator
        : std::make_shared<RecordingPageAllocator>();
    auto pages = current ? current->recording_pages
                         : std::shared_ptr<RecordingPageNode const>{};
    auto runs = current ? current->recording_runs
                        : std::shared_ptr<RecordingPageRunNode const>{};
    write_recording_region(pages, runs, *allocator, record);
    auto coverage = current ? current->coverage : IndexCoverage{};
    coverage.include(record.range);
    auto logical_const = make_recording_regions(
        record.output,
        coverage,
        record.layout,
        record.geometry,
        allocator,
        runs);
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
        .recording_pages = std::move(pages),
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
    IndexCoverage const& domain,
    std::span<GlobalIndexRange const> logical_order,
    std::vector<PersistedSampleRegion> regions)
{
    if (!successor_) throw std::logic_error("persisted data candidate is empty");
    if (!store_->owns_output(output) || output.kind() != PortKind::sample) {
        throw std::invalid_argument(
            "sample Region batch requires an output handle from its data store");
    }
    for (auto const& region : regions) {
        validate(region);
        if (region.output != output || region.layout != layout
            || region.geometry != geometry
            || !domain.contains(region.range)) {
            throw std::invalid_argument(
                "sample Region batch lies outside its replacement domain");
        }
    }
    if (std::ranges::any_of(
            logical_order,
            [](GlobalIndexRange region) { return region.empty(); })) {
        throw std::invalid_argument(
            "sample Region logical order contains an empty Region");
    }

    auto const output_index =
        PersistedDataStore::output_directory_index(output);
    auto const* current = find_output<SampleOutputNode, SampleOutputState>(
        successor_->sample_outputs_,
        successor_->sample_output_levels_,
        output_index);
    if (current && (current->layout != layout
            || current->geometry != geometry)) {
        throw std::invalid_argument(
            "persisted sample output Regions disagree on layout or geometry");
    }
    if (current && current->positional_recording) {
        throw std::invalid_argument(
            "persisted sample output cannot mix positional recording with authored Regions");
    }

    auto const previous_size = current && current->logical_regions
        ? current->logical_regions->size()
        : 0;
    std::vector<PersistedSampleRegion> retained;
    retained.reserve(previous_size);
    if (current && current->logical_regions) {
        for (auto const& existing : *current->logical_regions) {
            auto const intersects = domain.intersects(existing.range);
            if (intersects && !domain.contains(existing.range)) {
                throw std::invalid_argument(
                    "sample Region replacement domain clips an old logical Region");
            }
            if (!intersects) retained.push_back(existing);
        }
    }

    std::vector<bool> used_replacements(regions.size(), false);
    std::vector<bool> used_retained(retained.size(), false);
    std::vector<PersistedSampleRegion> next;
    next.reserve(logical_order.size());
    for (auto const expected : logical_order) {
        auto const replacement = [&]() -> std::optional<std::size_t> {
            for (std::size_t index = 0; index < regions.size(); ++index) {
                if (!used_replacements[index]
                    && regions[index].range == expected) {
                    return index;
                }
            }
            return std::nullopt;
        }();
        if (replacement) {
            used_replacements[*replacement] = true;
            next.push_back(std::move(regions[*replacement]));
            continue;
        }
        auto const previous = [&]() -> std::optional<std::size_t> {
            for (std::size_t index = 0; index < retained.size(); ++index) {
                if (!used_retained[index]
                    && retained[index].range == expected) {
                    return index;
                }
            }
            return std::nullopt;
        }();
        if (!previous && domain.intersects(expected)) {
            throw std::invalid_argument(
                "sample Region replacement cannot realize the published logical order");
        }
        if (!previous) continue;
        used_retained[*previous] = true;
        next.push_back(std::move(retained[*previous]));
    }
    if (std::ranges::contains(used_replacements, false)
        || std::ranges::contains(used_retained, false)) {
        throw std::invalid_argument(
            "sample Region replacement has records outside the published logical order");
    }

    if (next.empty()) {
        successor_->sample_region_count_ -= previous_size;
        successor_->sample_outputs_ = erase_output_node<SampleOutputNode>(
            successor_->sample_outputs_,
            successor_->sample_output_levels_,
            output_index);
        return;
    }

    std::shared_ptr<RangeNode<PersistedSampleRegion> const> root{};
    IndexCoverage coverage;
    for (auto const& region : next) {
        coverage.include(region.range);
        region.range.for_each_signed_order_segment(
            [&](GlobalIndexRange segment) {
                root = put_record(
                    root,
                    std::make_shared<PersistedSampleRegion const>(
                        slice(region, segment)));
            });
    }
    auto logical = std::make_shared<std::vector<PersistedSampleRegion> const>(
        std::move(next));
    auto const logical_count = logical->size();
    std::size_t physical_count = 0;
    for_each_record(root, [&](PersistedSampleRegion const&) {
        ++physical_count;
    });
    auto region_views = make_sample_region_views(logical);
    successor_->sample_region_count_ =
        successor_->sample_region_count_ - previous_size + logical_count;
    auto state = std::make_shared<SampleOutputState const>(SampleOutputState{
        .output = output,
        .coverage = std::move(coverage),
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
    IndexCoverage const& domain,
    std::span<GlobalIndexRange const> logical_order,
    std::vector<PersistedEventChunk> regions)
{
    if (!successor_) throw std::logic_error("persisted data candidate is empty");
    if (!store_->owns_output(output) || output.kind() != PortKind::event) {
        throw std::invalid_argument(
            "event Region batch requires an output handle from its data store");
    }
    for (auto const& region : regions) {
        validate(region);
        if (region.output != output || region.type != type
            || !domain.contains(region.range)) {
            throw std::invalid_argument(
                "event Region batch lies outside its replacement domain");
        }
    }
    if (std::ranges::any_of(
            logical_order,
            [](GlobalIndexRange region) { return region.empty(); })) {
        throw std::invalid_argument(
            "event Region logical order contains an empty Region");
    }

    auto const output_index =
        PersistedDataStore::output_directory_index(output);
    auto const* current = find_output<EventOutputNode, EventOutputState>(
        successor_->event_outputs_,
        successor_->event_output_levels_,
        output_index);
    if (current && current->type != type) {
        throw std::invalid_argument(
            "persisted event output Regions disagree on event type");
    }
    if (current && current->positional_recording) {
        throw std::invalid_argument(
            "persisted event output cannot mix positional recording with authored Regions");
    }

    std::vector<PersistedEventChunk> retained;
    if (current && current->logical_regions) {
        retained.reserve(current->logical_regions->size());
        for (auto const& existing : *current->logical_regions) {
            auto const intersects = domain.intersects(existing.range);
            if (intersects && !domain.contains(existing.range)) {
                throw std::invalid_argument(
                    "event Region replacement domain clips an old logical Region");
            }
            if (!intersects) retained.push_back(existing);
        }
    }

    std::vector<bool> used_replacements(regions.size(), false);
    std::vector<bool> used_retained(retained.size(), false);
    std::vector<PersistedEventChunk> next;
    next.reserve(logical_order.size());
    for (auto const expected : logical_order) {
        auto replacement = std::optional<std::size_t>{};
        for (std::size_t index = 0; index < regions.size(); ++index) {
            if (!used_replacements[index]
                && regions[index].range == expected) {
                replacement = index;
                break;
            }
        }
        if (replacement) {
            used_replacements[*replacement] = true;
            next.push_back(std::move(regions[*replacement]));
            continue;
        }
        auto previous = std::optional<std::size_t>{};
        for (std::size_t index = 0; index < retained.size(); ++index) {
            if (!used_retained[index]
                && retained[index].range == expected) {
                previous = index;
                break;
            }
        }
        if (!previous && domain.intersects(expected)) {
            throw std::invalid_argument(
                "event Region replacement cannot realize the published logical order");
        }
        if (!previous) continue;
        used_retained[*previous] = true;
        next.push_back(std::move(retained[*previous]));
    }
    if (std::ranges::contains(used_replacements, false)
        || std::ranges::contains(used_retained, false)) {
        throw std::invalid_argument(
            "event Region replacement has records outside the published logical order");
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
        coverage.include(region.range);
        region.range.for_each_signed_order_segment(
            [&](GlobalIndexRange segment) {
                remove_range(root, physical_count, segment);
                root = put_record(
                    root,
                    std::make_shared<PersistedEventChunk const>(
                        slice(region, segment)));
                ++physical_count;
            });
    }
    auto logical = std::make_shared<std::vector<PersistedEventChunk> const>(
        std::move(next));
    auto region_ranges = make_event_region_ranges(logical);
    auto timeline = make_event_timeline(logical);
    successor_->event_chunk_count_ =
        successor_->event_chunk_count_ - previous_count + physical_count;
    auto state = std::make_shared<EventOutputState const>(EventOutputState{
        .output = output,
        .coverage = std::move(coverage),
        .type = type,
        .chunks = std::move(root),
        .logical_regions = std::move(logical),
        .region_ranges = std::move(region_ranges),
        .timeline = std::move(timeline),
        .positional_recording = false,
        .chunk_count = physical_count,
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
        auto logical = make_recording_regions(
            output,
            coverage,
            layout,
            current->geometry,
            current->recording_allocator,
            current->recording_runs);
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
                .recording_pages = current->recording_pages,
                .recording_runs = current->recording_runs,
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
    if (!slot_) return;
    slot_->pinned.store(nullptr, std::memory_order_seq_cst);
    slot_ = nullptr;
    snapshot_ = nullptr;
}

PersistedDataStore::ReaderPin::~ReaderPin()
{
    reset();
}

PersistedDataStore::ReaderPin::ReaderPin(ReaderPin&& other) noexcept
    : slot_(std::exchange(other.slot_, nullptr))
    , snapshot_(std::exchange(other.snapshot_, nullptr))
{}

PersistedDataStore::ReaderPin& PersistedDataStore::ReaderPin::operator=(
    ReaderPin&& other) noexcept
{
    if (this == &other) return *this;
    reset();
    slot_ = std::exchange(other.slot_, nullptr);
    snapshot_ = std::exchange(other.snapshot_, nullptr);
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

PersistedDataStore::PersistedDataStore()
    : output_owner_token_(allocate_persisted_output_owner())
    , published_owner_([this] {
        auto snapshot = std::make_unique<Snapshot>();
        snapshot->output_owner_token_ = output_owner_token_;
        return snapshot;
    }())
    , published_(published_owner_.get())
{}

PersistedDataStore::~PersistedDataStore()
{
    assert(reader_slots_.empty());
}

PersistedDataStore::Candidate PersistedDataStore::begin_candidate(
    std::uint64_t target_semantic_version)
{
    auto const* base = published_.load(std::memory_order_seq_cst);
    if (target_semantic_version < base->version_.semantic) {
        throw std::invalid_argument(
            "persisted data candidate semantic version cannot move backwards");
    }
    if (base->version_.revision == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("persisted data revision is exhausted");
    }

    auto successor = std::make_unique<Snapshot>();
    successor->version_ = {
        .semantic = target_semantic_version,
        .revision = base->version_.revision + 1,
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
    if (current != candidate.base_
        || current->version() != candidate.base_version_) {
        candidate = {};
        return PersistedDataPublishResult::stale_base;
    }

    retired_.push_back(std::move(published_owner_));
    published_owner_ = std::move(candidate.successor_);
    published_.store(published_owner_.get(), std::memory_order_seq_cst);
    candidate.store_ = nullptr;
    candidate.base_ = nullptr;
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
