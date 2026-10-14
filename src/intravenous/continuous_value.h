#pragma once

#include <intravenous/global_index.h>
#include <intravenous/sample.h>

#include <array>
#include <bit>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace iv {

// A transported coordinate retains all 64 bits of the circular integer global
// index and adds 64 fractional bits.  It is graph data, not the coordinate used
// to address graph storage.
struct FractionalGlobalIndex {
    GlobalIndex integer {};
    std::uint64_t fraction = 0;

    constexpr bool operator==(FractionalGlobalIndex const&) const = default;
};

using FFTBin = std::complex<Sample::storage>;

// One rank-one frequency-domain continuous value.  Its complete node-facing
// view is defined together with type-specialized port access; the registry
// needs only its stable C++ identity here.  Keeping it incomplete prevents an
// accidental empty marker object's sizeof from becoming storage semantics.
class FFTBlock;

// This is the sole declaration site for a registered continuous value type.
// Its expansion derives the stable runtime ID, type trait, runtime descriptor,
// type-domain membership and capability sets. Adding a value type must add one
// declaration here; consumers must not grow type-specific switches.
#define IV_CONTINUOUS_VALUE_TYPES(X)                                      \
    X(sample, Sample, "Sample", 0, audio)                                \
    X(fractional_global_index, FractionalGlobalIndex,                    \
        "FractionalGlobalIndex", 0, scalar)                             \
    X(fft_block, FFTBlock, "FFTBlock", 1, audio)

enum class ContinuousValueTypeId : std::uint8_t {
#define IV_CONTINUOUS_VALUE_TYPE_ID(key, cpp_type, display_name, value_rank, cap) key,
    IV_CONTINUOUS_VALUE_TYPES(IV_CONTINUOUS_VALUE_TYPE_ID)
#undef IV_CONTINUOUS_VALUE_TYPE_ID
    count,
};

enum class ContinuousValueChannelCapability : std::uint8_t {
    scalar,
    audio,
};

struct ContinuousValueTypeInfo {
    ContinuousValueTypeId id {};
    std::string_view name {};
    std::size_t rank = 0;
    ContinuousValueChannelCapability channel_capability =
        ContinuousValueChannelCapability::scalar;
};

inline constexpr auto continuous_value_type_registry = std::array {
#define IV_CONTINUOUS_VALUE_TYPE_INFO(key, cpp_type, display_name, value_rank, cap) \
    ContinuousValueTypeInfo {                                            \
        .id = ContinuousValueTypeId::key,                                \
        .name = display_name,                                            \
        .rank = value_rank,                                              \
        .channel_capability =                                            \
            ContinuousValueChannelCapability::cap,                       \
    },
    IV_CONTINUOUS_VALUE_TYPES(IV_CONTINUOUS_VALUE_TYPE_INFO)
#undef IV_CONTINUOUS_VALUE_TYPE_INFO
};

static_assert(
    continuous_value_type_registry.size()
    == static_cast<std::size_t>(ContinuousValueTypeId::count));
static_assert([] {
    for (std::size_t index = 0;
         index < continuous_value_type_registry.size(); ++index) {
        if (continuous_value_type_registry[index].id
            != static_cast<ContinuousValueTypeId>(index)) {
            return false;
        }
    }
    return true;
}());
static_assert(std::is_trivially_copyable_v<FFTBin>);

[[nodiscard]] constexpr bool is_registered_continuous_value_type(
    ContinuousValueTypeId id) noexcept
{
    return static_cast<std::size_t>(id)
        < static_cast<std::size_t>(ContinuousValueTypeId::count);
}

[[nodiscard]] constexpr ContinuousValueTypeInfo const& continuous_value_type_info(
    ContinuousValueTypeId id)
{
    if (!is_registered_continuous_value_type(id)) {
        throw "unregistered continuous value type";
    }
    return continuous_value_type_registry[static_cast<std::size_t>(id)];
}

template<class T>
struct ContinuousValueType;

#define IV_DEFINE_CONTINUOUS_VALUE_TYPE(key, cpp_type, display_name, value_rank, cap) \
    template<>                                                           \
    struct ContinuousValueType<cpp_type> {                               \
        static constexpr auto id = ContinuousValueTypeId::key;           \
        static constexpr std::string_view name = display_name;           \
        static constexpr std::size_t rank = value_rank;                  \
        static constexpr auto channel_capability =                       \
            ContinuousValueChannelCapability::cap;                       \
    };
IV_CONTINUOUS_VALUE_TYPES(IV_DEFINE_CONTINUOUS_VALUE_TYPE)
#undef IV_DEFINE_CONTINUOUS_VALUE_TYPE

template<class T>
concept RegisteredContinuousValue = requires {
    ContinuousValueType<std::remove_cvref_t<T>>::id;
};

template<RegisteredContinuousValue T>
inline constexpr ContinuousValueTypeId continuous_value_type_id_v =
    ContinuousValueType<std::remove_cvref_t<T>>::id;

class ContinuousValueTypeSet {
public:
    using mask_type = std::uint64_t;

private:
    using Mask = mask_type;
    static_assert(
        static_cast<unsigned>(ContinuousValueTypeId::count)
        < std::numeric_limits<Mask>::digits,
        "continuous value type domains require a wider mask representation");
    Mask mask_ = 0;

    static constexpr Mask valid_mask = static_cast<Mask>(
        (Mask{1} << static_cast<unsigned>(ContinuousValueTypeId::count))
        - Mask{1});

    static constexpr Mask bit(ContinuousValueTypeId id) noexcept
    {
        return static_cast<Mask>(Mask{1} << static_cast<unsigned>(id));
    }

public:
    constexpr ContinuousValueTypeSet() noexcept = default;

    constexpr explicit ContinuousValueTypeSet(ContinuousValueTypeId id)
        : mask_(is_registered_continuous_value_type(id) ? bit(id) : Mask{0})
    {
        if (!is_registered_continuous_value_type(id)) {
            throw "unregistered continuous value type";
        }
    }

    constexpr explicit ContinuousValueTypeSet(Mask mask, std::in_place_t)
        : mask_(mask)
    {
        if ((mask_ & ~valid_mask) != 0) {
            throw "continuous value type set contains an unregistered type";
        }
    }

    template<RegisteredContinuousValue... Types>
    [[nodiscard]] static consteval ContinuousValueTypeSet of()
    {
        ContinuousValueTypeSet result;
        ((result.mask_ |= bit(continuous_value_type_id_v<Types>)), ...);
        static_assert(sizeof...(Types) != 0,
            "a continuous value type domain must not be empty");
        return result;
    }

    [[nodiscard]] static constexpr ContinuousValueTypeSet all() noexcept
    {
        return ContinuousValueTypeSet(valid_mask, std::in_place);
    }

    [[nodiscard]] static constexpr ContinuousValueTypeSet audio_capable() noexcept
    {
        ContinuousValueTypeSet result;
        for (auto const& type : continuous_value_type_registry) {
            if (type.channel_capability
                == ContinuousValueChannelCapability::audio) {
                result.mask_ |= bit(type.id);
            }
        }
        return result;
    }

    [[nodiscard]] constexpr bool empty() const noexcept { return mask_ == 0; }

    [[nodiscard]] constexpr std::size_t size() const noexcept
    {
        return std::popcount(mask_);
    }

    [[nodiscard]] constexpr bool contains(ContinuousValueTypeId id) const noexcept
    {
        return is_registered_continuous_value_type(id)
            && (mask_ & bit(id)) != 0;
    }

    template<RegisteredContinuousValue T>
    [[nodiscard]] constexpr bool contains() const noexcept
    {
        return contains(continuous_value_type_id_v<T>);
    }

    [[nodiscard]] constexpr std::optional<ContinuousValueTypeId> sole_type()
        const noexcept
    {
        if (size() != 1) return std::nullopt;
        return static_cast<ContinuousValueTypeId>(std::countr_zero(mask_));
    }

    [[nodiscard]] constexpr Mask bits() const noexcept { return mask_; }

    constexpr ContinuousValueTypeSet& operator&=(
        ContinuousValueTypeSet other) noexcept
    {
        mask_ &= other.mask_;
        return *this;
    }

    friend constexpr ContinuousValueTypeSet operator&(
        ContinuousValueTypeSet lhs,
        ContinuousValueTypeSet rhs) noexcept
    {
        lhs &= rhs;
        return lhs;
    }

    constexpr bool operator==(ContinuousValueTypeSet const&) const = default;
};

template<RegisteredContinuousValue... Types>
inline constexpr ContinuousValueTypeSet continuous_value_types =
    ContinuousValueTypeSet::of<Types...>();

inline constexpr ContinuousValueTypeSet sample_value_type =
    continuous_value_types<Sample>;

} // namespace iv
