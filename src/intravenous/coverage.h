#pragma once

#include <intravenous/ports.h>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

namespace iv {

struct IndexRegion {
    SampleIndex begin = 0;
    SampleIndex end = 0;

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return begin <= end;
    }

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return begin == end;
    }

    [[nodiscard]] constexpr bool contains(SampleIndex index) const noexcept
    {
        return begin <= index && index < end;
    }

    [[nodiscard]] constexpr bool contains(IndexRegion other) const noexcept
    {
        return other.empty()
            ? other.valid()
            : begin <= other.begin && other.end <= end;
    }

    constexpr auto operator<=>(IndexRegion const&) const = default;
};

class Coverage {
    std::vector<IndexRegion> regions_;

    void canonicalize()
    {
        std::erase_if(regions_, [](IndexRegion region) {
            return !region.valid() || region.empty();
        });
        std::ranges::sort(regions_);

        std::size_t write = 0;
        for (IndexRegion region : regions_) {
            if (write != 0 && region.begin <= regions_[write - 1].end) {
                regions_[write - 1].end =
                    std::max(regions_[write - 1].end, region.end);
                continue;
            }
            regions_[write++] = region;
        }
        regions_.resize(write);
    }

public:
    using Region = IndexRegion;
    using const_iterator = std::vector<Region>::const_iterator;

    Coverage() = default;

    explicit Coverage(Region region)
    {
        if (region.valid() && !region.empty()) regions_.push_back(region);
    }

    explicit Coverage(std::span<Region const> regions)
        : regions_(regions.begin(), regions.end())
    {
        canonicalize();
    }

    Coverage(std::initializer_list<Region> regions)
        : Coverage(std::span<Region const>{regions.begin(), regions.size()})
    {}

    [[nodiscard]] bool empty() const noexcept { return regions_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return regions_.size(); }

    [[nodiscard]] std::span<Region const> regions() const noexcept
    {
        return regions_;
    }

    [[nodiscard]] bool contains(SampleIndex index) const noexcept
    {
        auto const found = std::ranges::upper_bound(
            regions_, index, {}, &Region::begin);
        return found != regions_.begin() && std::prev(found)->contains(index);
    }

    [[nodiscard]] bool contains(Region region) const noexcept
    {
        if (!region.valid()) return false;
        if (region.empty()) return true;
        auto const found = std::ranges::upper_bound(
            regions_, region.begin, {}, &Region::begin);
        return found != regions_.begin() && std::prev(found)->contains(region);
    }

    [[nodiscard]] bool intersects(Region region) const noexcept
    {
        if (!region.valid() || region.empty()) return false;
        auto const found = std::ranges::lower_bound(
            regions_, region.end, {}, &Region::begin);
        return found != regions_.begin() && std::prev(found)->end > region.begin;
    }

    void include(Region region)
    {
        if (!region.valid() || region.empty()) return;

        auto first = std::ranges::lower_bound(
            regions_, region.begin, {}, &Region::end);
        while (first != regions_.end() && first->end < region.begin) ++first;

        auto last = first;
        while (last != regions_.end() && last->begin <= region.end) {
            region.begin = std::min(region.begin, last->begin);
            region.end = std::max(region.end, last->end);
            ++last;
        }
        regions_.erase(first, last);
        regions_.insert(
            std::ranges::lower_bound(regions_, region.begin, {}, &Region::begin),
            region);
    }

    void include(Coverage const& other)
    {
        if (other.empty()) return;
        std::vector<Region> combined;
        combined.reserve(regions_.size() + other.regions_.size());
        std::ranges::merge(regions_, other.regions_, std::back_inserter(combined));
        regions_ = std::move(combined);
        canonicalize();
    }

    void exclude(Region excluded)
    {
        if (!excluded.valid() || excluded.empty() || regions_.empty()) return;
        std::vector<Region> result;
        result.reserve(regions_.size() + 1);
        for (Region region : regions_) {
            if (region.end <= excluded.begin || excluded.end <= region.begin) {
                result.push_back(region);
                continue;
            }
            if (region.begin < excluded.begin) {
                result.push_back({region.begin, excluded.begin});
            }
            if (excluded.end < region.end) {
                result.push_back({excluded.end, region.end});
            }
        }
        regions_ = std::move(result);
    }

    void exclude(Coverage const& other)
    {
        for (Region region : other) exclude(region);
    }

    [[nodiscard]] Coverage intersection(
        Coverage const& other) const
    {
        Coverage result;
        auto left = begin();
        auto right = other.begin();
        while (left != end() && right != other.end()) {
            SampleIndex const overlap_begin = std::max(left->begin, right->begin);
            SampleIndex const overlap_end = std::min(left->end, right->end);
            if (overlap_begin < overlap_end) {
                result.regions_.push_back({overlap_begin, overlap_end});
            }
            if (left->end < right->end) ++left;
            else ++right;
        }
        return result;
    }

    [[nodiscard]] Coverage difference(Coverage const& other) const
    {
        Coverage result = *this;
        result.exclude(other);
        return result;
    }

    [[nodiscard]] const_iterator begin() const noexcept { return regions_.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return regions_.end(); }

    bool operator==(Coverage const&) const = default;
};

inline Coverage operator|(
    Coverage left, Coverage const& right)
{
    left.include(right);
    return left;
}

inline Coverage operator|(
    Coverage left, IndexRegion const& right)
{
    left.include(right);
    return left;
}

inline Coverage operator|(
    IndexRegion const& left, Coverage right)
{
    right.include(left);
    return right;
}

inline Coverage operator&(
    Coverage const& left, Coverage const& right)
{
    return left.intersection(right);
}

inline Coverage operator-(
    Coverage const& left, Coverage const& right)
{
    return left.difference(right);
}

inline Coverage operator-(
    Coverage left, IndexRegion const& right)
{
    left.exclude(right);
    return left;
}

} // namespace iv
