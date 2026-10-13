#pragma once

#include <intravenous/global_index.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <utility>

namespace iv {

// Exact positional coverage in the circular GlobalIndex domain. This is set
// algebra: overlap and multiplicity are intentionally discarded. Data-bearing
// random-access sample regions use the distinct SampleCoverage algebra.
//
// The canonical ranges form an immutable path-copied AVL tree in signed global
// index order. IndexCoverage copies therefore share their complete history,
// while a mutation replaces only paths to ranges that the mutation touches.
// Ordered iteration uses a fixed-depth stack and never allocates.
class IndexCoverage {
    struct Boundary {
        GlobalIndex::storage_type key = 0;
        bool domain_end = false;
    };

    struct Segment {
        GlobalIndex::storage_type begin = 0;
        Boundary end{};
    };

    struct Node;
    using NodePtr = std::shared_ptr<Node const>;

    struct Node {
        GlobalIndexRange range{};
        NodePtr left{};
        NodePtr right{};
        std::size_t subtree_size = 1;
        unsigned height = 1;
    };

    struct Segments {
        std::array<Segment, 2> values{};
        std::size_t size = 0;

        void push_back(Segment segment) noexcept
        {
            assert(size < values.size());
            values[size++] = segment;
        }
    };

    NodePtr root_{};

    [[nodiscard]] static constexpr bool less(
        Boundary left, Boundary right) noexcept
    {
        if (left.domain_end != right.domain_end) return !left.domain_end;
        return !left.domain_end && left.key < right.key;
    }

    [[nodiscard]] static constexpr bool less(
        Boundary left, GlobalIndex::storage_type right) noexcept
    {
        return !left.domain_end && left.key < right;
    }

    [[nodiscard]] static constexpr bool less(
        GlobalIndex::storage_type left, Boundary right) noexcept
    {
        return right.domain_end || left < right.key;
    }

    [[nodiscard]] static constexpr Boundary maximum(
        Boundary left, Boundary right) noexcept
    {
        return less(left, right) ? right : left;
    }

    [[nodiscard]] static constexpr Boundary minimum(
        Boundary left, Boundary right) noexcept
    {
        return less(left, right) ? left : right;
    }

    [[nodiscard]] static constexpr GlobalIndex from_signed_key(
        GlobalIndex::storage_type key) noexcept
    {
        constexpr auto sign = GlobalIndex::storage_type{1} << 63;
        return GlobalIndex::from_bits(key ^ sign);
    }

    [[nodiscard]] static Segments segments_of(GlobalIndexRange range) noexcept
    {
        Segments result;
        if (range.empty()) return result;

        auto const begin = SignedGlobalIndexOrder::key(range.begin);
        auto const distance_to_domain_end = begin == 0
            ? std::numeric_limits<GlobalIndex::extent_type>::max()
            : GlobalIndex::extent_type{0} - begin;

        // A begin of zero has 2^64 positions until the end of the signed
        // domain, one more than extent_type can represent. Every legal extent
        // therefore remains in the first segment.
        if (begin == 0 || range.extent < distance_to_domain_end) {
            result.push_back(Segment{
                .begin = begin,
                .end = Boundary{.key = begin + range.extent},
            });
            return result;
        }

        if (range.extent == distance_to_domain_end) {
            result.push_back(Segment{
                .begin = begin,
                .end = Boundary{.domain_end = true},
            });
            return result;
        }

        result.push_back(Segment{
            .begin = begin,
            .end = Boundary{.domain_end = true},
        });
        result.push_back(Segment{
            .begin = 0,
            .end = Boundary{.key = range.extent - distance_to_domain_end},
        });
        return result;
    }

    [[nodiscard]] static Segment segment_of(GlobalIndexRange range) noexcept
    {
        auto const begin = SignedGlobalIndexOrder::key(range.begin);
        auto const distance_to_domain_end = GlobalIndex::extent_type{0} - begin;
        if (begin != 0 && range.extent == distance_to_domain_end) {
            return {
                .begin = begin,
                .end = Boundary{.domain_end = true},
            };
        }
        return {
            .begin = begin,
            .end = Boundary{.key = begin + range.extent},
        };
    }

    [[nodiscard]] static GlobalIndexRange range_of(Segment segment) noexcept
    {
        assert(less(segment.begin, segment.end));
        assert(segment.begin != 0 || !segment.end.domain_end);
        return {
            .begin = from_signed_key(segment.begin),
            .extent = segment.end.domain_end
                ? GlobalIndex::extent_type{0} - segment.begin
                : segment.end.key - segment.begin,
        };
    }

    [[nodiscard]] static std::size_t size(NodePtr const& node) noexcept
    {
        return node ? node->subtree_size : 0;
    }

    [[nodiscard]] static unsigned height(NodePtr const& node) noexcept
    {
        return node ? node->height : 0;
    }

    [[nodiscard]] static NodePtr make_node(
        GlobalIndexRange range, NodePtr left = {}, NodePtr right = {})
    {
        auto node = std::make_shared<Node>();
        node->range = range;
        node->left = std::move(left);
        node->right = std::move(right);
        node->subtree_size = 1 + size(node->left) + size(node->right);
        node->height = 1 + std::max(height(node->left), height(node->right));
        return node;
    }

    [[nodiscard]] static int balance_factor(NodePtr const& node) noexcept
    {
        return static_cast<int>(height(node->left))
            - static_cast<int>(height(node->right));
    }

    [[nodiscard]] static NodePtr rotate_left(NodePtr const& node)
    {
        assert(node && node->right);
        auto const& promoted = node->right;
        auto demoted = make_node(
            node->range, node->left, promoted->left);
        return make_node(promoted->range, std::move(demoted), promoted->right);
    }

    [[nodiscard]] static NodePtr rotate_right(NodePtr const& node)
    {
        assert(node && node->left);
        auto const& promoted = node->left;
        auto demoted = make_node(
            node->range, promoted->right, node->right);
        return make_node(promoted->range, promoted->left, std::move(demoted));
    }

    [[nodiscard]] static NodePtr rebalance(NodePtr node)
    {
        auto const balance = balance_factor(node);
        if (balance > 1) {
            if (balance_factor(node->left) < 0) {
                node = make_node(
                    node->range, rotate_left(node->left), node->right);
            }
            return rotate_right(node);
        }
        if (balance < -1) {
            if (balance_factor(node->right) > 0) {
                node = make_node(
                    node->range, node->left, rotate_right(node->right));
            }
            return rotate_left(node);
        }
        return node;
    }

    [[nodiscard]] static NodePtr insert_exact(
        NodePtr const& node, GlobalIndexRange range)
    {
        if (!node) return make_node(range);
        auto const key = SignedGlobalIndexOrder::key(range.begin);
        auto const node_key = SignedGlobalIndexOrder::key(node->range.begin);
        if (key < node_key) {
            return rebalance(make_node(
                node->range,
                insert_exact(node->left, range),
                node->right));
        }
        if (node_key < key) {
            return rebalance(make_node(
                node->range,
                node->left,
                insert_exact(node->right, range)));
        }
        return make_node(range, node->left, node->right);
    }

    [[nodiscard]] static Node const* first(NodePtr const& node) noexcept
    {
        auto* selected = node.get();
        while (selected && selected->left) selected = selected->left.get();
        return selected;
    }

    [[nodiscard]] static NodePtr erase_exact(
        NodePtr const& node, GlobalIndex::storage_type key)
    {
        if (!node) return {};
        auto const node_key = SignedGlobalIndexOrder::key(node->range.begin);
        if (key < node_key) {
            auto left = erase_exact(node->left, key);
            if (left == node->left) return node;
            return rebalance(make_node(
                node->range, std::move(left), node->right));
        }
        if (node_key < key) {
            auto right = erase_exact(node->right, key);
            if (right == node->right) return node;
            return rebalance(make_node(
                node->range, node->left, std::move(right)));
        }
        if (!node->left) return node->right;
        if (!node->right) return node->left;
        auto const* successor = first(node->right);
        return rebalance(make_node(
            successor->range,
            node->left,
            erase_exact(
                node->right,
                SignedGlobalIndexOrder::key(successor->range.begin))));
    }

    [[nodiscard]] static Node const* predecessor(
        NodePtr const& root, GlobalIndex::storage_type key) noexcept
    {
        Node const* result = nullptr;
        auto const* node = root.get();
        while (node) {
            auto const node_key = SignedGlobalIndexOrder::key(node->range.begin);
            if (key < node_key) {
                node = node->left.get();
            } else {
                result = node;
                node = node->right.get();
            }
        }
        return result;
    }

    [[nodiscard]] static Node const* lower_bound(
        NodePtr const& root, GlobalIndex::storage_type key) noexcept
    {
        Node const* result = nullptr;
        auto const* node = root.get();
        while (node) {
            auto const node_key = SignedGlobalIndexOrder::key(node->range.begin);
            if (node_key < key) {
                node = node->right.get();
            } else {
                result = node;
                node = node->left.get();
            }
        }
        return result;
    }

    void insert_segment(Segment segment)
    {
        assert(less(segment.begin, segment.end));
        if (segment.begin == 0 && segment.end.domain_end) {
            constexpr auto half = GlobalIndex::extent_type{1} << 63;
            root_ = insert_exact(root_, range_of(Segment{
                .begin = 0,
                .end = Boundary{.key = half},
            }));
            root_ = insert_exact(root_, range_of(Segment{
                .begin = half,
                .end = Boundary{.domain_end = true},
            }));
            return;
        }
        root_ = insert_exact(root_, range_of(segment));
    }

    void include_segment(Segment included)
    {
        auto merged = included;
        if (auto const* previous = predecessor(root_, merged.begin)) {
            auto const previous_segment = segment_of(previous->range);
            if (!less(previous_segment.end, merged.begin)) {
                merged.begin = previous_segment.begin;
                merged.end = maximum(merged.end, previous_segment.end);
                root_ = erase_exact(root_, previous_segment.begin);
            }
        }

        while (auto const* next = lower_bound(root_, merged.begin)) {
            auto const next_segment = segment_of(next->range);
            if (less(merged.end, next_segment.begin)) break;
            merged.end = maximum(merged.end, next_segment.end);
            root_ = erase_exact(root_, next_segment.begin);
        }
        insert_segment(merged);
    }

    void exclude_segment(Segment excluded)
    {
        auto const* selected = predecessor(root_, excluded.begin);
        if (!selected
            || !less(excluded.begin, segment_of(selected->range).end)) {
            selected = lower_bound(root_, excluded.begin);
        }

        while (selected) {
            auto const current = segment_of(selected->range);
            if (!less(current.begin, excluded.end)) break;

            root_ = erase_exact(root_, current.begin);
            if (current.begin < excluded.begin) {
                insert_segment(Segment{
                    .begin = current.begin,
                    .end = Boundary{.key = excluded.begin},
                });
            }
            if (less(excluded.end, current.end)) {
                assert(!excluded.end.domain_end);
                insert_segment(Segment{
                    .begin = excluded.end.key,
                    .end = current.end,
                });
            }
            selected = lower_bound(root_, excluded.begin);
        }
    }

    [[nodiscard]] bool contains_segment(Segment requested) const noexcept
    {
        auto const* selected = predecessor(root_, requested.begin);
        if (!selected) return false;
        auto covered = segment_of(selected->range);
        if (requested.begin < covered.begin
            || !less(requested.begin, covered.end)) {
            return false;
        }

        while (less(covered.end, requested.end)) {
            if (covered.end.domain_end) return false;
            auto const* next = lower_bound(root_, covered.end.key);
            if (!next) return false;
            auto const next_segment = segment_of(next->range);
            if (next_segment.begin != covered.end.key) return false;
            covered.end = next_segment.end;
        }
        return true;
    }

    [[nodiscard]] bool intersects_segment(Segment requested) const noexcept
    {
        if (auto const* previous = predecessor(root_, requested.begin)) {
            if (less(requested.begin, segment_of(previous->range).end)) {
                return true;
            }
        }
        auto const* next = lower_bound(root_, requested.begin);
        return next && less(segment_of(next->range).begin, requested.end);
    }

public:
    using Range = GlobalIndexRange;

    class const_iterator {
        // An AVL tree addressable by size_t cannot reach twice the bit width
        // in height. The extra slot keeps this true at the empty-tree edge.
        static constexpr auto stack_capacity =
            std::numeric_limits<std::size_t>::digits * 2 + 1;

        std::array<Node const*, stack_capacity> stack_{};
        std::size_t depth_ = 0;

        void push_left(Node const* node) noexcept
        {
            while (node) {
                assert(depth_ < stack_.size());
                stack_[depth_++] = node;
                node = node->left.get();
            }
        }

    public:
        using value_type = Range;
        using reference = Range const&;
        using pointer = Range const*;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;
        using iterator_concept = std::forward_iterator_tag;

        const_iterator() noexcept = default;

        explicit const_iterator(Node const* root) noexcept
        {
            push_left(root);
        }

        [[nodiscard]] reference operator*() const noexcept
        {
            assert(depth_ != 0);
            return stack_[depth_ - 1]->range;
        }

        [[nodiscard]] pointer operator->() const noexcept
        {
            return std::addressof(**this);
        }

        const_iterator& operator++() noexcept
        {
            assert(depth_ != 0);
            auto const* selected = stack_[--depth_];
            push_left(selected->right.get());
            return *this;
        }

        const_iterator operator++(int) noexcept
        {
            auto previous = *this;
            ++*this;
            return previous;
        }

        [[nodiscard]] friend bool operator==(
            const_iterator const& left,
            const_iterator const& right) noexcept
        {
            auto const* left_node = left.depth_ == 0
                ? nullptr : left.stack_[left.depth_ - 1];
            auto const* right_node = right.depth_ == 0
                ? nullptr : right.stack_[right.depth_ - 1];
            return left_node == right_node;
        }
    };

    IndexCoverage() = default;

    explicit IndexCoverage(Range range)
    {
        include(range);
    }

    explicit IndexCoverage(std::span<Range const> ranges)
    {
        for (auto range : ranges) include(range);
    }

    IndexCoverage(std::initializer_list<Range> ranges)
        : IndexCoverage(std::span<Range const>{ranges.begin(), ranges.size()})
    {}

    [[nodiscard]] bool empty() const noexcept { return !root_; }
    [[nodiscard]] std::size_t size() const noexcept { return size(root_); }

    [[nodiscard]] bool contains(GlobalIndex index) const noexcept
    {
        auto const key = SignedGlobalIndexOrder::key(index);
        auto const* selected = predecessor(root_, key);
        return selected && selected->range.contains(index);
    }

    [[nodiscard]] bool contains(Range range) const noexcept
    {
        auto const segments = segments_of(range);
        for (std::size_t i = 0; i < segments.size; ++i) {
            if (!contains_segment(segments.values[i])) return false;
        }
        return true;
    }

    [[nodiscard]] bool intersects(Range range) const noexcept
    {
        auto const segments = segments_of(range);
        for (std::size_t i = 0; i < segments.size; ++i) {
            if (intersects_segment(segments.values[i])) return true;
        }
        return false;
    }

    void include(Range range)
    {
        auto const segments = segments_of(range);
        for (std::size_t i = 0; i < segments.size; ++i) {
            include_segment(segments.values[i]);
        }
    }

    void include(IndexCoverage const& other)
    {
        if (root_ == other.root_) return;
        for (auto range : other) include(range);
    }

    void exclude(Range excluded)
    {
        auto const segments = segments_of(excluded);
        for (std::size_t i = 0; i < segments.size; ++i) {
            exclude_segment(segments.values[i]);
        }
    }

    void exclude(IndexCoverage const& other)
    {
        if (root_ == other.root_) {
            root_.reset();
            return;
        }
        for (auto range : other) exclude(range);
    }

    [[nodiscard]] IndexCoverage intersection(
        IndexCoverage const& other) const
    {
        if (root_ == other.root_) return *this;
        IndexCoverage result;
        auto left = begin();
        auto right = other.begin();
        auto const left_end = end();
        auto const right_end = other.end();
        while (left != left_end && right != right_end) {
            auto const left_segment = segment_of(*left);
            auto const right_segment = segment_of(*right);
            auto const overlap_begin = std::max(
                left_segment.begin, right_segment.begin);
            auto const overlap_end = minimum(
                left_segment.end, right_segment.end);
            if (less(overlap_begin, overlap_end)) {
                result.include_segment(Segment{
                    .begin = overlap_begin,
                    .end = overlap_end,
                });
            }
            if (less(left_segment.end, right_segment.end)) ++left;
            else ++right;
        }
        return result;
    }

    [[nodiscard]] IndexCoverage difference(
        IndexCoverage const& other) const
    {
        IndexCoverage result = *this;
        result.exclude(other);
        return result;
    }

    [[nodiscard]] const_iterator begin() const noexcept
    {
        return const_iterator{root_.get()};
    }

    [[nodiscard]] const_iterator end() const noexcept { return {}; }

    [[nodiscard]] friend bool operator==(
        IndexCoverage const& left, IndexCoverage const& right) noexcept
    {
        if (left.root_ == right.root_) return true;
        if (left.size() != right.size()) return false;
        auto left_range = left.begin();
        auto right_range = right.begin();
        auto const left_end = left.end();
        while (left_range != left_end) {
            if (*left_range != *right_range) return false;
            ++left_range;
            ++right_range;
        }
        return true;
    }
};

inline IndexCoverage operator|(
    IndexCoverage left, IndexCoverage const& right)
{
    left.include(right);
    return left;
}

inline IndexCoverage operator|(
    IndexCoverage left, GlobalIndexRange const& right)
{
    left.include(right);
    return left;
}

inline IndexCoverage operator|(
    GlobalIndexRange const& left, IndexCoverage right)
{
    right.include(left);
    return right;
}

inline IndexCoverage operator&(
    IndexCoverage const& left, IndexCoverage const& right)
{
    return left.intersection(right);
}

inline IndexCoverage operator-(
    IndexCoverage const& left, IndexCoverage const& right)
{
    return left.difference(right);
}

inline IndexCoverage operator-(
    IndexCoverage left, GlobalIndexRange const& right)
{
    left.exclude(right);
    return left;
}

} // namespace iv
