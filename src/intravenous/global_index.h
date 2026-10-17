#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
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

    // Signed-canonical storage has one ordering seam between INT64_MAX and
    // INT64_MIN. Visit the one or two signed-ordered segments comprising this
    // modular range, preserving their forward range-relative order.
    template<typename Fn>
    constexpr void for_each_signed_order_segment(Fn&& fn) const;

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

template<typename Fn>
constexpr void GlobalIndexRange::for_each_signed_order_segment(Fn&& fn) const
{
    if (empty()) return;
    auto const begin_key = SignedGlobalIndexOrder::key(begin);
    if (begin_key == 0) {
        fn(*this);
        return;
    }

    auto const until_signed_domain_end =
        GlobalIndex::extent_type{0} - begin_key;
    if (extent <= until_signed_domain_end) {
        fn(*this);
        return;
    }

    fn(GlobalIndexRange{
        .begin = begin,
        .extent = until_signed_domain_end,
    });
    fn(GlobalIndexRange{
        .begin = begin.advanced_by(until_signed_domain_end),
        .extent = extent - until_signed_domain_end,
    });
}

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

// Unsigned Q64.64 interval extent. Unlike a coordinate, this is linear and may
// therefore be ordered. The all-zero representation is the only empty extent;
// a complete 2^128 execution cycle is deliberately not representable.
struct FractionalGlobalExtent {
    std::uint64_t whole = 0;
    std::uint64_t fraction = 0;

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return whole == 0 && fraction == 0;
    }

    [[nodiscard]] constexpr double project_samples() const noexcept
    {
        constexpr auto fractional_scale =
            1.0 / 18446744073709551616.0; // 2^64
        return static_cast<double>(whole)
            + static_cast<double>(fraction) * fractional_scale;
    }

    constexpr bool operator==(FractionalGlobalExtent const&) const noexcept = default;
};

[[nodiscard]] constexpr bool operator<(
    FractionalGlobalExtent left,
    FractionalGlobalExtent right) noexcept
{
    return left.whole < right.whole
        || (left.whole == right.whole && left.fraction < right.fraction);
}

// One coordinate in the circular Q64.64 global domain. It is used both to
// address fractional execution/storage positions and as a transported graph
// value. Context supplies that role; the coordinate arithmetic is identical.
class FractionalGlobalIndex {
public:
    constexpr FractionalGlobalIndex() noexcept = default;

    [[nodiscard]] static constexpr FractionalGlobalIndex from_parts(
        GlobalIndex whole,
        std::uint64_t fraction) noexcept
    {
        return FractionalGlobalIndex(whole, fraction);
    }

    [[nodiscard]] static constexpr FractionalGlobalIndex from_global(
        GlobalIndex index) noexcept
    {
        return from_parts(index, 0);
    }

    [[nodiscard]] constexpr GlobalIndex whole() const noexcept
    {
        return whole_;
    }

    [[nodiscard]] constexpr std::uint64_t fraction() const noexcept
    {
        return fraction_;
    }

    [[nodiscard]] constexpr FractionalGlobalIndex advanced_by(
        FractionalGlobalExtent extent) const noexcept
    {
        auto const fraction = fraction_ + extent.fraction;
        auto const carry = fraction < fraction_ ? std::uint64_t{1} : std::uint64_t{0};
        return from_parts(
            GlobalIndex::from_bits(whole_.bits() + extent.whole + carry),
            fraction);
    }

    [[nodiscard]] constexpr FractionalGlobalIndex retreated_by(
        FractionalGlobalExtent extent) const noexcept
    {
        auto const borrow = fraction_ < extent.fraction
            ? std::uint64_t{1}
            : std::uint64_t{0};
        return from_parts(
            GlobalIndex::from_bits(whole_.bits() - extent.whole - borrow),
            fraction_ - extent.fraction);
    }

    [[nodiscard]] constexpr FractionalGlobalExtent forward_distance_to(
        FractionalGlobalIndex other) const noexcept
    {
        auto const borrow = other.fraction_ < fraction_
            ? std::uint64_t{1}
            : std::uint64_t{0};
        return {
            .whole = other.whole_.bits() - whole_.bits() - borrow,
            .fraction = other.fraction_ - fraction_,
        };
    }

    constexpr bool operator==(FractionalGlobalIndex const&) const noexcept = default;

private:
    constexpr FractionalGlobalIndex(
        GlobalIndex whole,
        std::uint64_t fraction) noexcept
        : whole_(whole), fraction_(fraction)
    {}

    GlobalIndex whole_ {};
    std::uint64_t fraction_ = 0;
};

static_assert(sizeof(FractionalGlobalIndex) == 2 * sizeof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<FractionalGlobalIndex>);
static_assert(std::is_standard_layout_v<FractionalGlobalIndex>);

// A finite half-open interval in the circular Q64.64 execution domain.
struct FractionalGlobalIndexRange {
    struct LatticeProjection {
        FractionalGlobalIndex begin {};
        // Low 64 bits of the modular value ordinal on this lattice. Ring
        // addressing needs only these bits even when a fast lattice contains
        // more than 2^64 positions in one complete global-index cycle.
        std::uint64_t begin_ordinal_bits = 0;
        std::size_t count = 0;

        constexpr bool operator==(LatticeProjection const&) const noexcept = default;
    };

    static constexpr std::int32_t minimum_pace_exponent = -64;
    static constexpr std::int32_t maximum_pace_exponent = 64;

    FractionalGlobalIndex begin {};
    FractionalGlobalExtent extent {};

    [[nodiscard]] static constexpr FractionalGlobalIndexRange from_global_range(
        GlobalIndexRange range) noexcept
    {
        return {
            .begin = FractionalGlobalIndex::from_global(range.begin),
            .extent = {.whole = range.extent},
        };
    }

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return extent.empty();
    }

    [[nodiscard]] constexpr FractionalGlobalIndex end() const noexcept
    {
        return begin.advanced_by(extent);
    }

    [[nodiscard]] constexpr bool contains(FractionalGlobalIndex index) const noexcept
    {
        return begin.forward_distance_to(index) < extent;
    }

    // Returns the first lattice address in this range and the number of lattice
    // values it contains. An empty optional means either the exponent is outside
    // the Q64.64 domain or the value count cannot fit in size_t.
    [[nodiscard]] constexpr std::optional<LatticeProjection>
    try_project_onto_lattice(std::int32_t pace_exponent) const noexcept;

    // Maximum number of values that any integer-aligned global interval of
    // this extent can expose on the selected lattice. Storage planning uses
    // the maximum because a slow lattice's actual count depends on the root
    // interval's phase. An empty optional means that the pace is outside the
    // Q64.64 domain or the count cannot fit in size_t.
    [[nodiscard]] static constexpr std::optional<std::size_t>
    maximum_projection_count(
        GlobalIndex::extent_type global_extent,
        std::int32_t pace_exponent) noexcept
    {
        if (pace_exponent < minimum_pace_exponent
            || pace_exponent > maximum_pace_exponent) {
            return std::nullopt;
        }
        if (global_extent == 0) return std::size_t{0};

        constexpr auto digits = std::numeric_limits<std::size_t>::digits;
        if (pace_exponent >= 0) {
            auto const shift = static_cast<std::uint32_t>(pace_exponent);
            if (shift >= digits
                || global_extent
                    > (std::numeric_limits<std::size_t>::max() >> shift)) {
                return std::nullopt;
            }
            return static_cast<std::size_t>(global_extent) << shift;
        }

        auto const shift = static_cast<std::uint32_t>(-pace_exponent);
        if (shift >= 64) return std::size_t{1};
        auto const spacing = std::uint64_t{1} << shift;
        auto const quotient = global_extent / spacing;
        auto const remainder = global_extent % spacing;
        if (quotient > std::numeric_limits<std::size_t>::max()
            || (quotient == std::numeric_limits<std::size_t>::max()
                && remainder != 0)) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(quotient)
            + static_cast<std::size_t>(remainder != 0);
    }
};

namespace detail {

[[nodiscard]] constexpr FractionalGlobalExtent subtract(
    FractionalGlobalExtent left,
    FractionalGlobalExtent right) noexcept
{
    auto const borrow = left.fraction < right.fraction
        ? std::uint64_t{1}
        : std::uint64_t{0};
    return {
        .whole = left.whole - right.whole - borrow,
        .fraction = left.fraction - right.fraction,
    };
}

[[nodiscard]] constexpr FractionalGlobalExtent alignment_offset(
    FractionalGlobalIndex index,
    std::uint32_t spacing_shift) noexcept
{
    if (spacing_shift == 0) return {};
    if (spacing_shift < 64) {
        auto const mask = (std::uint64_t{1} << spacing_shift) - 1;
        auto const remainder = index.fraction() & mask;
        return remainder == 0
            ? FractionalGlobalExtent{}
            : FractionalGlobalExtent{
                .fraction = (std::uint64_t{1} << spacing_shift) - remainder,
            };
    }
    if (spacing_shift == 64) {
        return index.fraction() == 0
            ? FractionalGlobalExtent{}
            : FractionalGlobalExtent{.fraction = std::uint64_t{0} - index.fraction()};
    }
    if (spacing_shift < 128) {
        auto const high_bits = spacing_shift - 64;
        auto const mask = (std::uint64_t{1} << high_bits) - 1;
        auto const remainder_high = index.whole().bits() & mask;
        auto const remainder_low = index.fraction();
        if (remainder_high == 0 && remainder_low == 0) return {};
        auto const borrow = remainder_low == 0 ? std::uint64_t{0} : std::uint64_t{1};
        return {
            .whole = (std::uint64_t{1} << high_bits) - remainder_high - borrow,
            .fraction = std::uint64_t{0} - remainder_low,
        };
    }

    if (index.whole().bits() == 0 && index.fraction() == 0) return {};
    auto const borrow = index.fraction() == 0 ? std::uint64_t{0} : std::uint64_t{1};
    return {
        .whole = std::uint64_t{0} - index.whole().bits() - borrow,
        .fraction = std::uint64_t{0} - index.fraction(),
    };
}

[[nodiscard]] constexpr FractionalGlobalExtent shift_right(
    FractionalGlobalExtent value,
    std::uint32_t shift) noexcept
{
    if (shift == 0) return value;
    if (shift < 64) {
        return {
            .whole = value.whole >> shift,
            .fraction = (value.fraction >> shift)
                | (value.whole << (64 - shift)),
        };
    }
    if (shift == 64) return {.fraction = value.whole};
    if (shift < 128) return {.fraction = value.whole >> (shift - 64)};
    return {};
}

[[nodiscard]] constexpr std::uint64_t lattice_ordinal_bits(
    FractionalGlobalIndex index,
    std::int32_t pace_exponent) noexcept
{
    if (pace_exponent <= 0) {
        auto const shift = static_cast<std::uint32_t>(-pace_exponent);
        return shift == 64 ? 0 : index.whole().bits() >> shift;
    }
    auto const shift = static_cast<std::uint32_t>(pace_exponent);
    if (shift == 64) return index.fraction();
    return (index.whole().bits() << shift)
        | (index.fraction() >> (64 - shift));
}

} // namespace detail

constexpr std::optional<FractionalGlobalIndexRange::LatticeProjection>
FractionalGlobalIndexRange::try_project_onto_lattice(
    std::int32_t pace_exponent) const noexcept
{
    if (pace_exponent < minimum_pace_exponent
        || pace_exponent > maximum_pace_exponent) {
        return std::nullopt;
    }
    if (empty()) {
        return LatticeProjection{
            .begin = begin,
            .begin_ordinal_bits = detail::lattice_ordinal_bits(
                begin, pace_exponent),
        };
    }

    auto const spacing_shift = static_cast<std::uint32_t>(64 - pace_exponent);
    auto const offset = detail::alignment_offset(begin, spacing_shift);
    if (!(offset < extent)) {
        // An empty projection still begins at its lattice lower bound. Its
        // ordinal is the insertion position of the first value after this
        // source interval, not the floor of an unaligned interval end.
        auto const projection_begin = begin.advanced_by(offset);
        return LatticeProjection{
            .begin = projection_begin,
            .begin_ordinal_bits = detail::lattice_ordinal_bits(
                projection_begin, pace_exponent),
        };
    }

    auto const first = begin.advanced_by(offset);
    auto after_first = detail::subtract(extent, offset);
    after_first = detail::subtract(
        after_first, FractionalGlobalExtent{.fraction = 1});
    auto const additional = detail::shift_right(after_first, spacing_shift);
    if (additional.whole != 0
        || additional.fraction >= std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    return LatticeProjection{
        .begin = first,
        .begin_ordinal_bits = detail::lattice_ordinal_bits(
            first, pace_exponent),
        .count = static_cast<std::size_t>(additional.fraction) + 1,
    };
}

} // namespace iv
