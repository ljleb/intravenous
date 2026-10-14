#include <intravenous/runtime/persisted_data_store.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace iv {
namespace persisted_data_store_detail {

template<typename Record>
struct RangeNode {
    std::shared_ptr<Record const> record{};
    std::shared_ptr<RangeNode const> left{};
    std::shared_ptr<RangeNode const> right{};
    std::uint32_t height = 1;
};

struct SampleOutputState {
    PersistedOutputHandle output{};
    IndexCoverage coverage{};
    ChannelLayout layout{};
    std::shared_ptr<RangeNode<PersistedSampleRegion> const> regions{};
    std::size_t region_count = 0;
};

struct EventOutputState {
    PersistedOutputHandle output{};
    IndexCoverage coverage{};
    EventTypeId type = EventTypeId::empty;
    std::shared_ptr<RangeNode<PersistedEventChunk> const> chunks{};
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

namespace {

using persisted_data_store_detail::EventOutputNode;
using persisted_data_store_detail::EventOutputState;
using persisted_data_store_detail::RangeNode;
using persisted_data_store_detail::SampleOutputNode;
using persisted_data_store_detail::SampleOutputState;

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
    auto const frames = checked_frame_count(region.range);
    auto const expected = checked_product(frames, channel_count(region.layout));
    if (region.values.size() != expected) {
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
        .values = std::vector<Sample>(checked_product(frames, channels)),
    };
    if (source.layout.sample_layout == SampleStreamLayout::planar) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            std::copy_n(
                source.values.begin()
                    + static_cast<std::ptrdiff_t>(
                        channel * source_frames + frame_offset),
                frames,
                result.values.begin()
                    + static_cast<std::ptrdiff_t>(channel * frames));
        }
    } else {
        auto const value_offset = checked_product(frame_offset, channels);
        auto const value_count = checked_product(frames, channels);
        std::copy_n(
            source.values.begin() + static_cast<std::ptrdiff_t>(value_offset),
            value_count,
            result.values.begin());
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

Sample PersistedSampleRegion::at(
    GlobalIndex index,
    std::size_t channel) const noexcept
{
    auto const frame = range.offset_of(index);
    auto const channels = channel_count(layout);
    if (!frame || channel >= channels
        || range.extent > std::numeric_limits<std::size_t>::max()) {
        return {};
    }
    auto const frames = static_cast<std::size_t>(range.extent);
    auto const frame_index = static_cast<std::size_t>(*frame);
    auto const offset = layout.sample_layout == SampleStreamLayout::planar
        ? channel * frames + frame_index
        : frame_index * channels + channel;
    return offset < values.size() ? values[offset] : Sample{};
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
    return selected ? find_record(selected->regions, index) : nullptr;
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
    return selected ? find_record(selected->chunks, index) : nullptr;
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
    if (!selected) return;
    range.for_each_signed_order_segment([&](GlobalIndexRange segment) {
        for_each_intersecting(selected->regions, segment, [&](auto const& record) {
            visitor(visitor_data, *record, intersection(record->range, segment));
        });
    });
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
    range.for_each_signed_order_segment([&](GlobalIndexRange segment) {
        for_each_intersecting(selected->chunks, segment, [&](auto const& record) {
            visitor(visitor_data, *record, intersection(record->range, segment));
        });
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
    if (current && current->layout != record.layout) {
        throw std::invalid_argument(
            "persisted sample output regions disagree on channel layout");
    }

    auto root = current ? current->regions
                        : std::shared_ptr<RangeNode<PersistedSampleRegion> const>{};
    auto count = current ? current->region_count : 0;
    auto coverage = current ? current->coverage : IndexCoverage{};
    auto const original_range = record.range;
    original_range.for_each_signed_order_segment([&](GlobalIndexRange segment) {
        remove_range(root, count, segment);
        auto replacement = slice(record, segment);
        root = put_record(
            root,
            std::make_shared<PersistedSampleRegion const>(
                std::move(replacement)));
        ++count;
    });
    coverage.include(original_range);

    auto const previous_count = current ? current->region_count : 0;
    successor_->sample_region_count_ =
        successor_->sample_region_count_ - previous_count + count;
    auto state = std::make_shared<SampleOutputState const>(SampleOutputState{
        .output = record.output,
        .coverage = std::move(coverage),
        .layout = record.layout,
        .regions = std::move(root),
        .region_count = count,
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

    auto const previous_count = current ? current->chunk_count : 0;
    successor_->event_chunk_count_ =
        successor_->event_chunk_count_ - previous_count + count;
    auto state = std::make_shared<EventOutputState const>(EventOutputState{
        .output = record.output,
        .coverage = std::move(coverage),
        .type = record.type,
        .chunks = std::move(root),
        .chunk_count = count,
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

    auto root = current->regions;
    auto count = current->region_count;
    range.for_each_signed_order_segment([&](GlobalIndexRange segment) {
        remove_range(root, count, segment);
    });
    successor_->sample_region_count_ =
        successor_->sample_region_count_ - current->region_count + count;
    if (count == 0) {
        successor_->sample_outputs_ = erase_output_node<SampleOutputNode>(
            successor_->sample_outputs_,
            successor_->sample_output_levels_,
            output_index);
        return;
    }
    auto coverage = current->coverage;
    coverage.exclude(range);
    auto state = std::make_shared<SampleOutputState const>(SampleOutputState{
        .output = output,
        .coverage = std::move(coverage),
        .layout = layout,
        .regions = std::move(root),
        .region_count = count,
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
    auto state = std::make_shared<EventOutputState const>(EventOutputState{
        .output = output,
        .coverage = std::move(coverage),
        .type = type,
        .chunks = std::move(root),
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
