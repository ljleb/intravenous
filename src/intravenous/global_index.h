#pragma once

#include <bit>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace iv {

// One integer coordinate in the circular global DSP timeline. The stored bits
// are always uint64_t and arithmetic is modulo 2^64. A signed interpretation is
// available explicitly for authoring, display, and canonical persisted order;
// it is not the timeline's universal ordering.
class GlobalIndex {
public:
    using storage_type = std::uint64_t;
    using signed_type = std::int64_t;
    using extent_type = std::uint64_t;
    using difference_type = std::int64_t;

    constexpr GlobalIndex() noexcept = default;

    [[nodiscard]] static constexpr GlobalIndex from_bits(
        storage_type bits) noexcept
    {
        return GlobalIndex{bits};
    }

    [[nodiscard]] static constexpr GlobalIndex from_signed(
        signed_type value) noexcept
    {
        return from_bits(static_cast<storage_type>(value));
    }

    [[nodiscard]] constexpr storage_type bits() const noexcept
    {
        return bits_;
    }

    [[nodiscard]] constexpr signed_type signed_value() const noexcept
    {
        return std::bit_cast<signed_type>(bits_);
    }

    explicit constexpr operator signed_type() const noexcept
    {
        return signed_value();
    }

    [[nodiscard]] constexpr GlobalIndex advanced_by(
        extent_type extent) const noexcept
    {
        return from_bits(bits_ + extent);
    }

    [[nodiscard]] constexpr GlobalIndex retreated_by(
        extent_type extent) const noexcept
    {
        return from_bits(bits_ - extent);
    }

    [[nodiscard]] constexpr GlobalIndex shifted_by(
        difference_type difference) const noexcept
    {
        return from_bits(bits_ + static_cast<storage_type>(difference));
    }

    [[nodiscard]] constexpr extent_type forward_distance_to(
        GlobalIndex other) const noexcept
    {
        return other.bits_ - bits_;
    }

    // Signed relative displacement in the half-domain centered on *this.
    // Callers must use it only when their finite horizon is known to fit in
    // int64_t; it is not a global ordering operation.
    [[nodiscard]] constexpr difference_type signed_distance_to(
        GlobalIndex other) const noexcept
    {
        return std::bit_cast<difference_type>(forward_distance_to(other));
    }

    constexpr GlobalIndex& operator++() noexcept
    {
        ++bits_;
        return *this;
    }

    constexpr GlobalIndex operator++(int) noexcept
    {
        auto previous = *this;
        ++*this;
        return previous;
    }

    constexpr bool operator==(GlobalIndex const&) const noexcept = default;

private:
    explicit constexpr GlobalIndex(storage_type bits) noexcept : bits_(bits) {}

    storage_type bits_ = 0;
};

static_assert(sizeof(GlobalIndex) == sizeof(GlobalIndex::storage_type));
static_assert(std::is_trivially_copyable_v<GlobalIndex>);
static_assert(std::is_standard_layout_v<GlobalIndex>);

// A finite half-open interval [begin, begin + extent) in forward modular
// timeline order. begin == end() is therefore not evidence that the range is
// empty; only extent == 0 is.
struct GlobalIndexRange {
    GlobalIndex begin{};
    GlobalIndex::extent_type extent = 0;

    [[nodiscard]] static constexpr GlobalIndexRange from_begin_and_end(
        GlobalIndex begin, GlobalIndex end) noexcept
    {
        return {
            .begin = begin,
            .extent = begin.forward_distance_to(end),
        };
    }

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return extent == 0;
    }

    [[nodiscard]] constexpr GlobalIndex end() const noexcept
    {
        return begin.advanced_by(extent);
    }

    [[nodiscard]] constexpr bool contains(GlobalIndex index) const noexcept
    {
        return begin.forward_distance_to(index) < extent;
    }

    [[nodiscard]] constexpr std::optional<GlobalIndex::extent_type> offset_of(
        GlobalIndex index) const noexcept
    {
        auto const offset = begin.forward_distance_to(index);
        if (offset >= extent) return std::nullopt;
        return offset;
    }

    [[nodiscard]] constexpr bool contains(GlobalIndexRange other) const noexcept
    {
        if (other.empty()) return true;
        auto const offset = offset_of(other.begin);
        return offset && other.extent <= extent - *offset;
    }

    constexpr bool operator==(GlobalIndexRange const&) const noexcept = default;
};

// Canonical signed presentation/persistence order. The XOR maps INT64_MIN to
// key zero and preserves increasing int64 order without signed arithmetic.
struct SignedGlobalIndexOrder {
    [[nodiscard]] static constexpr GlobalIndex::storage_type key(
        GlobalIndex index) noexcept
    {
        constexpr auto sign = GlobalIndex::storage_type{1} << 63;
        return index.bits() ^ sign;
    }

    [[nodiscard]] constexpr bool operator()(
        GlobalIndex left, GlobalIndex right) const noexcept
    {
        return key(left) < key(right);
    }
};

// Forward order relative to an explicitly selected origin. This is the order
// used for events and iteration inside a finite callback/query range.
struct RangeRelativeIndexOrder {
    GlobalIndex origin{};

    [[nodiscard]] constexpr bool operator()(
        GlobalIndex left, GlobalIndex right) const noexcept
    {
        return origin.forward_distance_to(left)
            < origin.forward_distance_to(right);
    }
};

} // namespace iv
