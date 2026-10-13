#pragma once

#include <intravenous/global_index.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace iv {

// Exact positional coverage in the circular GlobalIndex domain. This is set
// algebra: overlap and multiplicity are intentionally discarded. Data-bearing
// random-access sample regions use the distinct SampleCoverage algebra.
class IndexCoverage {
    struct Boundary {
        GlobalIndex::storage_type key = 0;
        bool domain_end = false;
    };

    struct Segment {
        GlobalIndex::storage_type begin = 0;
        Boundary end{};
    };

    std::vector<GlobalIndexRange> ranges_;

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

    static void append_segments(
        GlobalIndexRange range, std::vector<Segment>& destination)
    {
        if (range.empty()) return;

        auto const begin = SignedGlobalIndexOrder::key(range.begin);
        auto const distance_to_domain_end = begin == 0
            ? std::numeric_limits<GlobalIndex::extent_type>::max()
            : GlobalIndex::extent_type{0} - begin;

        // A begin of zero has 2^64 positions until the end of the signed
        // domain, one more than extent_type can represent. Every legal extent
        // therefore remains in the first segment.
        if (begin == 0 || range.extent < distance_to_domain_end) {
            destination.push_back(Segment{
                .begin = begin,
                .end = Boundary{.key = begin + range.extent},
            });
            return;
        }

        if (range.extent == distance_to_domain_end) {
            destination.push_back(Segment{
                .begin = begin,
                .end = Boundary{.domain_end = true},
            });
            return;
        }

        destination.push_back(Segment{
            .begin = begin,
            .end = Boundary{.domain_end = true},
        });
        destination.push_back(Segment{
            .begin = 0,
            .end = Boundary{.key = range.extent - distance_to_domain_end},
        });
    }

    [[nodiscard]] static std::vector<Segment> segments_of(
        std::span<GlobalIndexRange const> ranges)
    {
        std::vector<Segment> result;
        result.reserve(ranges.size() + 1);
        for (auto range : ranges) append_segments(range, result);
        std::ranges::sort(result, {}, &Segment::begin);
        return result;
    }

    void assign_segments(std::vector<Segment> segments)
    {
        std::erase_if(segments, [](Segment segment) {
            return !less(segment.begin, segment.end);
        });
        std::ranges::sort(segments, {}, &Segment::begin);

        std::size_t write = 0;
        for (auto segment : segments) {
            if (write != 0 && !less(segments[write - 1].end, segment.begin)) {
                segments[write - 1].end = maximum(
                    segments[write - 1].end, segment.end);
                continue;
            }
            segments[write++] = segment;
        }
        segments.resize(write);

        ranges_.clear();
        ranges_.reserve(segments.size() + 1);
        for (auto segment : segments) {
            if (!segment.end.domain_end) {
                ranges_.push_back(GlobalIndexRange{
                    .begin = from_signed_key(segment.begin),
                    .extent = segment.end.key - segment.begin,
                });
                continue;
            }

            if (segment.begin != 0) {
                ranges_.push_back(GlobalIndexRange{
                    .begin = from_signed_key(segment.begin),
                    .extent = GlobalIndex::extent_type{0} - segment.begin,
                });
                continue;
            }

            // The complete 2^64-position domain has no single representable
            // extent. Its unique canonical representation is two signed-order
            // half-domain ranges.
            constexpr auto half = GlobalIndex::extent_type{1} << 63;
            ranges_.push_back(GlobalIndexRange{
                .begin = from_signed_key(0),
                .extent = half,
            });
            ranges_.push_back(GlobalIndexRange{
                .begin = from_signed_key(half),
                .extent = half,
            });
        }
    }

    void canonicalize()
    {
        assign_segments(segments_of(ranges_));
    }

public:
    using Range = GlobalIndexRange;
    using const_iterator = std::vector<Range>::const_iterator;

    IndexCoverage() = default;

    explicit IndexCoverage(Range range) : ranges_{range}
    {
        canonicalize();
    }

    explicit IndexCoverage(std::span<Range const> ranges)
        : ranges_(ranges.begin(), ranges.end())
    {
        canonicalize();
    }

    IndexCoverage(std::initializer_list<Range> ranges)
        : IndexCoverage(std::span<Range const>{ranges.begin(), ranges.size()})
    {}

    [[nodiscard]] bool empty() const noexcept { return ranges_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return ranges_.size(); }

    [[nodiscard]] std::span<Range const> ranges() const noexcept
    {
        return ranges_;
    }

    [[nodiscard]] bool contains(GlobalIndex index) const noexcept
    {
        auto const key = SignedGlobalIndexOrder::key(index);
        auto const found = std::ranges::upper_bound(
            ranges_, key, {}, [](Range const& range) {
                return SignedGlobalIndexOrder::key(range.begin);
            });
        return found != ranges_.begin() && std::prev(found)->contains(index);
    }

    [[nodiscard]] bool contains(Range range) const
    {
        if (range.empty()) return true;
        for (auto segment : segments_of(std::span{&range, 1})) {
            auto const found = std::ranges::upper_bound(
                ranges_, segment.begin, {}, [](Range const& candidate) {
                    return SignedGlobalIndexOrder::key(candidate.begin);
                });
            if (found == ranges_.begin()) return false;
            std::vector<Segment> containing_segments;
            append_segments(*std::prev(found), containing_segments);
            auto const& containing = containing_segments.front();
            if (segment.begin < containing.begin
                || less(containing.end, segment.end)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool intersects(Range range) const
    {
        if (range.empty()) return false;
        auto const requested = segments_of(std::span{&range, 1});
        auto const stored = segments_of(ranges_);
        std::size_t left = 0;
        std::size_t right = 0;
        while (left < stored.size() && right < requested.size()) {
            auto const overlap_begin = std::max(
                stored[left].begin, requested[right].begin);
            auto const overlap_end = minimum(
                stored[left].end, requested[right].end);
            if (less(overlap_begin, overlap_end)) return true;
            if (less(stored[left].end, requested[right].end)) ++left;
            else ++right;
        }
        return false;
    }

    void include(Range range)
    {
        if (range.empty()) return;
        auto segments = segments_of(ranges_);
        append_segments(range, segments);
        assign_segments(std::move(segments));
    }

    void include(IndexCoverage const& other)
    {
        if (other.empty()) return;
        auto segments = segments_of(ranges_);
        auto appended = segments_of(other.ranges_);
        segments.insert(segments.end(), appended.begin(), appended.end());
        assign_segments(std::move(segments));
    }

    void exclude(Range excluded)
    {
        if (excluded.empty() || ranges_.empty()) return;
        auto current = segments_of(ranges_);
        for (auto removed : segments_of(std::span{&excluded, 1})) {
            std::vector<Segment> next;
            next.reserve(current.size() + 1);
            for (auto segment : current) {
                if (!less(segment.begin, removed.end)
                    || !less(removed.begin, segment.end)) {
                    next.push_back(segment);
                    continue;
                }
                if (segment.begin < removed.begin) {
                    next.push_back(Segment{
                        .begin = segment.begin,
                        .end = Boundary{.key = removed.begin},
                    });
                }
                if (less(removed.end, segment.end)) {
                    next.push_back(Segment{
                        .begin = removed.end.key,
                        .end = segment.end,
                    });
                }
            }
            current = std::move(next);
        }
        assign_segments(std::move(current));
    }

    void exclude(IndexCoverage const& other)
    {
        for (Range range : other) exclude(range);
    }

    [[nodiscard]] IndexCoverage intersection(
        IndexCoverage const& other) const
    {
        auto left = segments_of(ranges_);
        auto right = segments_of(other.ranges_);
        std::vector<Segment> result;
        std::size_t left_index = 0;
        std::size_t right_index = 0;
        while (left_index < left.size() && right_index < right.size()) {
            auto const overlap_begin = std::max(
                left[left_index].begin, right[right_index].begin);
            auto const overlap_end = minimum(
                left[left_index].end, right[right_index].end);
            if (less(overlap_begin, overlap_end)) {
                result.push_back(Segment{
                    .begin = overlap_begin,
                    .end = overlap_end,
                });
            }
            if (less(left[left_index].end, right[right_index].end)) {
                ++left_index;
            } else {
                ++right_index;
            }
        }
        IndexCoverage coverage;
        coverage.assign_segments(std::move(result));
        return coverage;
    }

    [[nodiscard]] IndexCoverage difference(
        IndexCoverage const& other) const
    {
        IndexCoverage result = *this;
        result.exclude(other);
        return result;
    }

    [[nodiscard]] const_iterator begin() const noexcept { return ranges_.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return ranges_.end(); }

    bool operator==(IndexCoverage const&) const = default;
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
