#include <intravenous/runtime/persisted_page_store.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <utility>

namespace iv {
namespace persisted_page_store_detail {

template<typename Page>
struct PageNode {
    std::shared_ptr<Page const> page{};
    std::shared_ptr<PageNode const> left{};
    std::shared_ptr<PageNode const> right{};
    std::uint32_t height = 1;
};

struct SampleOutputState {
    PersistedOutputHandle output{};
    Coverage coverage{};
    ChannelLayout layout{};
    std::shared_ptr<PageNode<PersistedSamplePage> const> pages{};
    std::size_t page_count = 0;
};

struct EventOutputState {
    PersistedOutputHandle output{};
    Coverage coverage{};
    EventTypeId type = EventTypeId::empty;
    std::shared_ptr<PageNode<PersistedEventPage> const> pages{};
    std::size_t page_count = 0;
};

template<typename State>
struct OutputDirectoryNode {
    using Node = OutputDirectoryNode<State>;
    using Children = std::array<std::shared_ptr<Node const>, 16>;
    using Values = std::array<std::shared_ptr<State const>, 16>;

    std::variant<Children, Values> entries{Values{}};
};

} // namespace persisted_page_store_detail

namespace {

using persisted_page_store_detail::EventOutputNode;
using persisted_page_store_detail::EventOutputState;
using persisted_page_store_detail::PageNode;
using persisted_page_store_detail::SampleOutputNode;
using persisted_page_store_detail::SampleOutputState;

std::atomic<std::uint32_t> next_persisted_output_owner{0};

[[nodiscard]] std::uint32_t allocate_persisted_output_owner()
{
    constexpr auto exclusive_limit =
        std::numeric_limits<std::uint32_t>::max() / 2;
    auto owner = next_persisted_output_owner.load(std::memory_order_relaxed);
    for (;;) {
        if (owner >= exclusive_limit) {
            throw std::overflow_error(
                "persisted page store owner tokens are exhausted");
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

template<typename Node>
[[nodiscard]] std::uint32_t node_height(
    std::shared_ptr<Node const> const& node) noexcept
{
    return node ? node->height : 0;
}

template<typename Page>
[[nodiscard]] std::shared_ptr<PageNode<Page> const> make_page_node(
    std::shared_ptr<Page const> page,
    std::shared_ptr<PageNode<Page> const> left = {},
    std::shared_ptr<PageNode<Page> const> right = {})
{
    auto const height = 1 + std::max(node_height(left), node_height(right));
    return std::make_shared<PageNode<Page> const>(PageNode<Page>{
        .page = std::move(page),
        .left = std::move(left),
        .right = std::move(right),
        .height = height,
    });
}

template<typename Page>
[[nodiscard]] std::shared_ptr<PageNode<Page> const> balance_page_node(
    std::shared_ptr<PageNode<Page> const> node)
{
    auto const balance = static_cast<int>(node_height(node->left))
        - static_cast<int>(node_height(node->right));
    if (balance > 1) {
        auto left = node->left;
        if (node_height(left->left) < node_height(left->right)) {
            auto const pivot = left->right;
            left = make_page_node<Page>(
                left->page,
                left->left,
                pivot->left);
            left = make_page_node<Page>(
                pivot->page,
                std::move(left),
                pivot->right);
        }
        auto const new_right = make_page_node<Page>(
            node->page, left->right, node->right);
        return make_page_node<Page>(
            left->page, left->left, std::move(new_right));
    }
    if (balance < -1) {
        auto right = node->right;
        if (node_height(right->right) < node_height(right->left)) {
            auto const pivot = right->left;
            right = make_page_node<Page>(
                right->page,
                pivot->right,
                right->right);
            right = make_page_node<Page>(
                pivot->page,
                pivot->left,
                std::move(right));
        }
        auto const new_left = make_page_node<Page>(
            node->page, node->left, right->left);
        return make_page_node<Page>(
            right->page, std::move(new_left), right->right);
    }
    return node;
}

template<typename Page>
[[nodiscard]] std::shared_ptr<PageNode<Page> const> put_page(
    std::shared_ptr<PageNode<Page> const> const& root,
    std::shared_ptr<Page const> page)
{
    if (!root) return make_page_node<Page>(std::move(page));
    if (page->page_index < root->page->page_index) {
        return balance_page_node<Page>(make_page_node<Page>(
            root->page,
            put_page<Page>(root->left, std::move(page)),
            root->right));
    }
    if (root->page->page_index < page->page_index) {
        return balance_page_node<Page>(make_page_node<Page>(
            root->page,
            root->left,
            put_page<Page>(root->right, std::move(page))));
    }
    return make_page_node<Page>(std::move(page), root->left, root->right);
}

template<typename Page>
[[nodiscard]] Page const* find_page(
    std::shared_ptr<PageNode<Page> const> const& root,
    std::uint64_t page_index) noexcept
{
    auto node = root.get();
    while (node) {
        if (page_index < node->page->page_index) node = node->left.get();
        else if (node->page->page_index < page_index) node = node->right.get();
        else return node->page.get();
    }
    return nullptr;
}

template<typename Page>
[[nodiscard]] std::shared_ptr<PageNode<Page> const> erase_page_node(
    std::shared_ptr<PageNode<Page> const> const& root,
    std::uint64_t page_index)
{
    if (!root) return {};
    if (page_index < root->page->page_index) {
        return balance_page_node<Page>(make_page_node<Page>(
            root->page,
            erase_page_node<Page>(root->left, page_index),
            root->right));
    }
    if (root->page->page_index < page_index) {
        return balance_page_node<Page>(make_page_node<Page>(
            root->page,
            root->left,
            erase_page_node<Page>(root->right, page_index)));
    }
    if (!root->left) return root->right;
    if (!root->right) return root->left;
    auto successor = root->right;
    while (successor->left) successor = successor->left;
    return balance_page_node<Page>(make_page_node<Page>(
        successor->page,
        root->left,
        erase_page_node<Page>(
            root->right, successor->page->page_index)));
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

[[nodiscard]] IndexRegion page_interval(
    std::uint64_t page_index,
    std::size_t page_width)
{
    auto const width = static_cast<SampleIndex>(page_width);
    auto const maximum = std::numeric_limits<SampleIndex>::max();
    if (page_width == 0 || page_index > maximum / width) {
        throw std::invalid_argument("persisted page index exceeds the sample timeline");
    }
    auto const begin = page_index * width;
    if (begin > maximum - width) {
        throw std::invalid_argument("persisted page interval exceeds the sample timeline");
    }
    return {begin, begin + width};
}

void validate_domain(Coverage const& domain, IndexRegion interval)
{
    if (domain.empty()) {
        throw std::invalid_argument("persisted page domain cannot be empty");
    }
    for (auto const region : domain.regions()) {
        if (!interval.contains(region)) {
            throw std::invalid_argument(
                "persisted page domain lies outside its canonical page interval");
        }
    }
}

[[nodiscard]] std::size_t covered_sample_count(Coverage const& coverage)
{
    std::size_t result = 0;
    for (auto const region : coverage.regions()) {
        auto const length = region.end - region.begin;
        if (length > std::numeric_limits<std::size_t>::max() - result) {
            throw std::invalid_argument("persisted page domain is too large");
        }
        result += static_cast<std::size_t>(length);
    }
    return result;
}

[[nodiscard]] std::size_t checked_product(
    std::size_t left,
    std::size_t right)
{
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
        throw std::invalid_argument("persisted sample page payload is too large");
    }
    return left * right;
}

void validate_page(PersistedSamplePage const& page, std::size_t page_width)
{
    if (!page.output.valid()
        || persisted_output_kind(page.output) != PortKind::sample) {
        throw std::invalid_argument("sample page requires a sample output identity");
    }
    if (!is_valid_channel_type(page.layout.channel_type)
        || !is_valid_sample_stream_layout(page.layout.sample_layout)) {
        throw std::invalid_argument("sample page has an invalid channel layout");
    }
    if (page.packing != PersistedSamplePacking::dense
        && page.packing != PersistedSamplePacking::coverage_packed) {
        throw std::invalid_argument("sample page has an invalid packing");
    }
    validate_domain(page.domain, page_interval(page.page_index, page_width));
    auto const frames = page.packing == PersistedSamplePacking::dense
        ? page_width
        : covered_sample_count(page.domain);
    auto const expected = checked_product(frames, channel_count(page.layout));
    if (page.values.size() != expected) {
        throw std::invalid_argument(
            "sample page payload does not match its packing, domain and layout");
    }
}

void validate_page(PersistedEventPage const& page, std::size_t page_width)
{
    if (!page.output.valid()
        || persisted_output_kind(page.output) != PortKind::event) {
        throw std::invalid_argument("event page requires an event output identity");
    }
    if (page.type >= EventTypeId::count) {
        throw std::invalid_argument("event page has an invalid event type");
    }
    auto const interval = page_interval(page.page_index, page_width);
    validate_domain(page.domain, interval);
    EventTime previous = 0;
    bool first = true;
    for (auto const& event : page.events) {
        if (event.time >= page_width) {
            throw std::invalid_argument("event lies outside its persisted page");
        }
        if (!page.domain.contains(
                interval.begin + static_cast<SampleIndex>(event.time))) {
            throw std::invalid_argument("event lies outside its persisted page domain");
        }
        auto const type_matches =
            (page.type == EventTypeId::empty
                && std::holds_alternative<EmptyEvent>(event.value))
            || (page.type == EventTypeId::trigger
                && std::holds_alternative<TriggerEvent>(event.value))
            || (page.type == EventTypeId::boundary
                && std::holds_alternative<BoundaryEvent>(event.value))
            || (page.type == EventTypeId::midi
                && std::holds_alternative<MidiEvent>(event.value));
        if (!type_matches) {
            throw std::invalid_argument(
                "event page payload does not match its event type");
        }
        if (!first && event.time < previous) {
            throw std::invalid_argument(
                "persisted event page events must be in nondecreasing time order");
        }
        previous = event.time;
        first = false;
    }
}

} // namespace

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
    if (planned.storage != graph_jit::PortStorageKind::persisted_pages
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

PersistedOutputHandle PersistedPageStore::resolve_output(
    PersistedOutputId const& output)
{
    std::lock_guard lock{output_slots_mutex_};
    auto const existing = std::ranges::find_if(
        output_slots_, [&](auto const& registered) {
            return registered.first == output;
        });
    if (existing != output_slots_.end()) return existing->second;

    if (output_slots_.size()
        > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error(
            "persisted page store output slots are exhausted");
    }
    auto const handle = PersistedOutputHandle{
        output_owner_token_,
        static_cast<std::uint32_t>(output_slots_.size()),
        persisted_output_kind(output)};
    output_slots_.push_back({output, handle});
    return handle;
}

std::uint32_t PersistedPageStore::output_owner_token(
    PersistedOutputHandle output) noexcept
{
    return static_cast<std::uint32_t>(
        output.value_ >> PersistedOutputHandle::owner_shift);
}

std::uint32_t PersistedPageStore::output_directory_index(
    PersistedOutputHandle output) noexcept
{
    return static_cast<std::uint32_t>(
        output.value_ >> PersistedOutputHandle::slot_shift);
}

bool PersistedPageStore::owns_output(
    PersistedOutputHandle output) const noexcept
{
    return output.valid()
        && output_owner_token(output) == output_owner_token_;
}

PersistedSamplePage const* PersistedPageStore::Snapshot::find_sample_page(
    PersistedOutputHandle output,
    std::uint64_t page_index) const noexcept
{
    if (!output.valid() || output.kind() != PortKind::sample
        || PersistedPageStore::output_owner_token(output)
            != output_owner_token_) {
        return nullptr;
    }
    auto const* selected = find_output<SampleOutputNode, SampleOutputState>(
        sample_outputs_,
        sample_output_levels_,
        PersistedPageStore::output_directory_index(output));
    return selected ? find_page(selected->pages, page_index) : nullptr;
}

PersistedEventPage const* PersistedPageStore::Snapshot::find_event_page(
    PersistedOutputHandle output,
    std::uint64_t page_index) const noexcept
{
    if (!output.valid() || output.kind() != PortKind::event
        || PersistedPageStore::output_owner_token(output)
            != output_owner_token_) {
        return nullptr;
    }
    auto const* selected = find_output<EventOutputNode, EventOutputState>(
        event_outputs_,
        event_output_levels_,
        PersistedPageStore::output_directory_index(output));
    return selected ? find_page(selected->pages, page_index) : nullptr;
}

Coverage const* PersistedPageStore::Snapshot::find_sample_coverage(
    PersistedOutputHandle output,
    ChannelLayout layout) const noexcept
{
    if (!output.valid() || output.kind() != PortKind::sample
        || PersistedPageStore::output_owner_token(output)
            != output_owner_token_) {
        return nullptr;
    }
    auto const* selected = find_output<SampleOutputNode, SampleOutputState>(
        sample_outputs_,
        sample_output_levels_,
        PersistedPageStore::output_directory_index(output));
    return selected && selected->layout == layout
        ? &selected->coverage
        : nullptr;
}

Coverage const* PersistedPageStore::Snapshot::find_event_coverage(
    PersistedOutputHandle output,
    EventTypeId type) const noexcept
{
    if (!output.valid() || output.kind() != PortKind::event
        || PersistedPageStore::output_owner_token(output)
            != output_owner_token_) {
        return nullptr;
    }
    auto const* selected = find_output<EventOutputNode, EventOutputState>(
        event_outputs_,
        event_output_levels_,
        PersistedPageStore::output_directory_index(output));
    return selected && selected->type == type
        ? &selected->coverage
        : nullptr;
}

PersistedPageStore::Candidate::Candidate(
    PersistedPageStore& store,
    Snapshot const& base,
    std::unique_ptr<Snapshot> successor) noexcept
    : store_(&store)
    , base_(&base)
    , base_version_(base.version())
    , successor_(std::move(successor))
{}

PersistedPageSnapshotVersion
PersistedPageStore::Candidate::target_version() const noexcept
{
    return successor_ ? successor_->version() : PersistedPageSnapshotVersion{};
}

std::size_t PersistedPageStore::Candidate::page_width() const noexcept
{
    return successor_ ? successor_->page_width() : 0;
}

PersistedPageStore::Snapshot const&
PersistedPageStore::Candidate::working_snapshot() const
{
    if (!successor_) throw std::logic_error("persisted page candidate is empty");
    return *successor_;
}

void PersistedPageStore::Candidate::put(PersistedSamplePage page)
{
    if (!successor_) throw std::logic_error("persisted page candidate is empty");
    validate_page(page, successor_->page_width_);
    if (!store_->owns_output(page.output)) {
        throw std::invalid_argument(
            "sample page output handle belongs to another persisted page store");
    }
    auto replacement = std::make_shared<PersistedSamplePage const>(std::move(page));
    auto const output_index =
        PersistedPageStore::output_directory_index(replacement->output);
    auto const* current = find_output<SampleOutputNode, SampleOutputState>(
        successor_->sample_outputs_,
        successor_->sample_output_levels_,
        output_index);
    if (current && current->layout != replacement->layout) {
        throw std::invalid_argument(
            "persisted sample output pages disagree on channel layout");
    }
    auto const* previous = current
        ? find_page(current->pages, replacement->page_index)
        : nullptr;
    Coverage coverage = current ? current->coverage : Coverage{};
    if (previous) coverage.exclude(previous->domain);
    coverage.include(replacement->domain);
    auto state = std::make_shared<SampleOutputState const>(SampleOutputState{
        .output = replacement->output,
        .coverage = std::move(coverage),
        .layout = replacement->layout,
        .pages = put_page(
            current
                ? current->pages
                : std::shared_ptr<PageNode<PersistedSamplePage> const>{},
            std::move(replacement)),
        .page_count = (current ? current->page_count : 0)
            + (previous ? 0 : 1),
    });
    successor_->sample_outputs_ = put_output<
        SampleOutputNode, SampleOutputState>(
            successor_->sample_outputs_,
            successor_->sample_output_levels_,
            output_index,
            std::move(state));
    if (!previous) ++successor_->sample_page_count_;
}

void PersistedPageStore::Candidate::put(PersistedEventPage page)
{
    if (!successor_) throw std::logic_error("persisted page candidate is empty");
    validate_page(page, successor_->page_width_);
    if (!store_->owns_output(page.output)) {
        throw std::invalid_argument(
            "event page output handle belongs to another persisted page store");
    }
    auto replacement = std::make_shared<PersistedEventPage const>(std::move(page));
    auto const output_index =
        PersistedPageStore::output_directory_index(replacement->output);
    auto const* current = find_output<EventOutputNode, EventOutputState>(
        successor_->event_outputs_,
        successor_->event_output_levels_,
        output_index);
    if (current && current->type != replacement->type) {
        throw std::invalid_argument(
            "persisted event output pages disagree on event type");
    }
    auto const* previous = current
        ? find_page(current->pages, replacement->page_index)
        : nullptr;
    Coverage coverage = current ? current->coverage : Coverage{};
    if (previous) coverage.exclude(previous->domain);
    coverage.include(replacement->domain);
    auto state = std::make_shared<EventOutputState const>(EventOutputState{
        .output = replacement->output,
        .coverage = std::move(coverage),
        .type = replacement->type,
        .pages = put_page(
            current
                ? current->pages
                : std::shared_ptr<PageNode<PersistedEventPage> const>{},
            std::move(replacement)),
        .page_count = (current ? current->page_count : 0)
            + (previous ? 0 : 1),
    });
    successor_->event_outputs_ = put_output<EventOutputNode, EventOutputState>(
        successor_->event_outputs_,
        successor_->event_output_levels_,
        output_index,
        std::move(state));
    if (!previous) ++successor_->event_page_count_;
}

void PersistedPageStore::Candidate::erase_page(
    PersistedOutputHandle output,
    std::uint64_t page_index)
{
    if (!successor_) throw std::logic_error("persisted page candidate is empty");
    if (!store_->owns_output(output)) {
        throw std::invalid_argument(
            "persisted page erasure requires an output handle from its store");
    }
    auto const output_index =
        PersistedPageStore::output_directory_index(output);
    if (persisted_output_kind(output) == PortKind::sample) {
        auto const* current = find_output<SampleOutputNode, SampleOutputState>(
            successor_->sample_outputs_,
            successor_->sample_output_levels_,
            output_index);
        auto const* previous = current
            ? find_page(current->pages, page_index)
            : nullptr;
        if (!previous) return;
        --successor_->sample_page_count_;
        if (current->page_count == 1) {
            successor_->sample_outputs_ = erase_output_node<
                SampleOutputNode>(
                    successor_->sample_outputs_,
                    successor_->sample_output_levels_,
                    output_index);
            return;
        }
        auto coverage = current->coverage;
        coverage.exclude(previous->domain);
        auto state = std::make_shared<SampleOutputState const>(
            SampleOutputState{
                .output = current->output,
                .coverage = std::move(coverage),
                .layout = current->layout,
                .pages = erase_page_node(current->pages, page_index),
                .page_count = current->page_count - 1,
            });
        successor_->sample_outputs_ = put_output<
            SampleOutputNode, SampleOutputState>(
                successor_->sample_outputs_,
                successor_->sample_output_levels_,
                output_index,
                std::move(state));
    } else {
        auto const* current = find_output<EventOutputNode, EventOutputState>(
            successor_->event_outputs_,
            successor_->event_output_levels_,
            output_index);
        auto const* previous = current
            ? find_page(current->pages, page_index)
            : nullptr;
        if (!previous) return;
        --successor_->event_page_count_;
        if (current->page_count == 1) {
            successor_->event_outputs_ = erase_output_node<
                EventOutputNode>(
                    successor_->event_outputs_,
                    successor_->event_output_levels_,
                    output_index);
            return;
        }
        auto coverage = current->coverage;
        coverage.exclude(previous->domain);
        auto state = std::make_shared<EventOutputState const>(EventOutputState{
            .output = current->output,
            .coverage = std::move(coverage),
            .type = current->type,
            .pages = erase_page_node(current->pages, page_index),
            .page_count = current->page_count - 1,
        });
        successor_->event_outputs_ = put_output<EventOutputNode, EventOutputState>(
            successor_->event_outputs_,
            successor_->event_output_levels_,
            output_index,
            std::move(state));
    }
}

void PersistedPageStore::Candidate::erase_output(
    PersistedOutputHandle output)
{
    if (!successor_) throw std::logic_error("persisted page candidate is empty");
    if (!store_->owns_output(output)) {
        throw std::invalid_argument(
            "persisted output erasure requires an output handle from its store");
    }
    auto const output_index =
        PersistedPageStore::output_directory_index(output);
    if (persisted_output_kind(output) == PortKind::sample) {
        auto const* current = find_output<SampleOutputNode, SampleOutputState>(
            successor_->sample_outputs_,
            successor_->sample_output_levels_,
            output_index);
        if (!current) return;
        successor_->sample_page_count_ -= current->page_count;
        successor_->sample_outputs_ = erase_output_node<
            SampleOutputNode>(
                successor_->sample_outputs_,
                successor_->sample_output_levels_,
                output_index);
    } else {
        auto const* current = find_output<EventOutputNode, EventOutputState>(
            successor_->event_outputs_,
            successor_->event_output_levels_,
            output_index);
        if (!current) return;
        successor_->event_page_count_ -= current->page_count;
        successor_->event_outputs_ = erase_output_node<
            EventOutputNode>(
                successor_->event_outputs_,
                successor_->event_output_levels_,
                output_index);
    }
}

PersistedPageStore::ReaderPin::ReaderPin(
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

void PersistedPageStore::ReaderPin::reset() noexcept
{
    if (!slot_) return;
    slot_->pinned.store(nullptr, std::memory_order_seq_cst);
    slot_ = nullptr;
    snapshot_ = nullptr;
}

PersistedPageStore::ReaderPin::~ReaderPin()
{
    reset();
}

PersistedPageStore::ReaderPin::ReaderPin(ReaderPin&& other) noexcept
    : slot_(std::exchange(other.slot_, nullptr))
    , snapshot_(std::exchange(other.snapshot_, nullptr))
{}

PersistedPageStore::ReaderPin& PersistedPageStore::ReaderPin::operator=(
    ReaderPin&& other) noexcept
{
    if (this == &other) return *this;
    reset();
    slot_ = std::exchange(other.slot_, nullptr);
    snapshot_ = std::exchange(other.snapshot_, nullptr);
    return *this;
}

PersistedPageStore::Snapshot const&
PersistedPageStore::ReaderPin::snapshot() const noexcept
{
    assert(snapshot_ != nullptr);
    return *snapshot_;
}

PersistedPageStore::ReaderSlot::ReaderSlot(
    PersistedPageStore& store,
    ReaderSlotState& state) noexcept
    : store_(&store)
    , state_(&state)
{}

void PersistedPageStore::ReaderSlot::reset() noexcept
{
    if (!state_) return;
    store_->unregister_reader(*state_);
    store_ = nullptr;
    state_ = nullptr;
}

PersistedPageStore::ReaderSlot::~ReaderSlot()
{
    reset();
}

PersistedPageStore::ReaderSlot::ReaderSlot(ReaderSlot&& other) noexcept
    : store_(std::exchange(other.store_, nullptr))
    , state_(std::exchange(other.state_, nullptr))
{}

PersistedPageStore::ReaderSlot& PersistedPageStore::ReaderSlot::operator=(
    ReaderSlot&& other) noexcept
{
    if (this == &other) return *this;
    reset();
    store_ = std::exchange(other.store_, nullptr);
    state_ = std::exchange(other.state_, nullptr);
    return *this;
}

PersistedPageStore::ReaderPin PersistedPageStore::ReaderSlot::pin() noexcept
{
    assert(store_ != nullptr && state_ != nullptr);
    return ReaderPin{*state_, store_->published_};
}

PersistedPageStore::PersistedPageStore()
    : output_owner_token_(allocate_persisted_output_owner())
    , published_owner_([this] {
        auto snapshot = std::make_unique<Snapshot>();
        snapshot->output_owner_token_ = output_owner_token_;
        return snapshot;
    }())
    , published_(published_owner_.get())
{}

PersistedPageStore::~PersistedPageStore()
{
    assert(reader_slots_.empty());
}

PersistedPageStore::Candidate PersistedPageStore::begin_candidate(
    std::uint64_t target_semantic_version,
    std::size_t page_width)
{
    if (page_width == 0) {
        throw std::invalid_argument("persisted page width must be nonzero");
    }
    auto const* base = published_.load(std::memory_order_seq_cst);
    if (target_semantic_version < base->version_.semantic) {
        throw std::invalid_argument(
            "persisted page candidate semantic version cannot move backwards");
    }
    if (base->version_.page
        == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("persisted page version is exhausted");
    }
    if (base->page_width_ != 0 && base->page_width_ != page_width
        && (base->sample_page_count_ != 0 || base->event_page_count_ != 0)) {
        throw std::invalid_argument(
            "persisted pages must be explicitly repaged before changing page width");
    }

    auto successor = std::make_unique<Snapshot>();
    successor->version_ = {
        .semantic = target_semantic_version,
        .page = base->version_.page + 1,
    };
    successor->page_width_ = page_width;
    successor->output_owner_token_ = base->output_owner_token_;
    successor->sample_outputs_ = base->sample_outputs_;
    successor->sample_output_levels_ = base->sample_output_levels_;
    successor->event_outputs_ = base->event_outputs_;
    successor->event_output_levels_ = base->event_output_levels_;
    successor->sample_page_count_ = base->sample_page_count_;
    successor->event_page_count_ = base->event_page_count_;
    return Candidate{*this, *base, std::move(successor)};
}

PersistedPagePublishResult PersistedPageStore::publish(Candidate&& candidate)
{
    if (candidate.store_ != this || !candidate.successor_) {
        throw std::invalid_argument(
            "persisted page candidate does not belong to this store");
    }
    auto const* current = published_.load(std::memory_order_seq_cst);
    if (current != candidate.base_
        || current->version() != candidate.base_version_) {
        candidate = {};
        return PersistedPagePublishResult::stale_base;
    }

    retired_.push_back(std::move(published_owner_));
    published_owner_ = std::move(candidate.successor_);
    published_.store(published_owner_.get(), std::memory_order_seq_cst);
    candidate.store_ = nullptr;
    candidate.base_ = nullptr;
    return PersistedPagePublishResult::published;
}

PersistedPageStore::ReaderSlot PersistedPageStore::register_reader()
{
    auto state = std::make_unique<ReaderSlotState>();
    auto* pointer = state.get();
    std::lock_guard lock{reader_slots_mutex_};
    reader_slots_.push_back(std::move(state));
    return ReaderSlot{*this, *pointer};
}

void PersistedPageStore::unregister_reader(ReaderSlotState& state) noexcept
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

std::size_t PersistedPageStore::reclaim_retired()
{
    std::lock_guard lock{reader_slots_mutex_};
    // A reader announces before loading published_. If reclamation observes an
    // acquisition in flight it defers the whole scan; otherwise that reader's
    // later published_ load can only select the current, non-retired root.
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
