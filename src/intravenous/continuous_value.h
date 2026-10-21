#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/global_index.h>
#include <intravenous/sample.h>

#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace iv {

using FFTBin = std::complex<Sample::storage>;

// One canonical nonredundant real-transform spectrum. FFTBlock is a read-only
// value view; MutableFFTBlock is the corresponding authored-output view. The
// transported storage is the referenced FFTBin sequence, never the span object
// itself. Keeping the view here makes the value extension self-contained and
// prevents an FFT backend from leaking its packed/native representation into
// graph storage or node source.
class FFTBlock {
    std::span<FFTBin const> bins_ {};

public:
    constexpr FFTBlock() noexcept = default;
    constexpr explicit FFTBlock(std::span<FFTBin const> bins) noexcept
        : bins_(bins)
    {}

    [[nodiscard]] constexpr FFTBin const* data() const noexcept
    {
        return bins_.data();
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept
    {
        return bins_.size();
    }

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return bins_.empty();
    }

    [[nodiscard]] constexpr FFTBin const& operator[](
        std::size_t index) const noexcept
    {
        return bins_[index];
    }

    [[nodiscard]] constexpr auto begin() const noexcept
    {
        return bins_.begin();
    }

    [[nodiscard]] constexpr auto end() const noexcept
    {
        return bins_.end();
    }

    [[nodiscard]] constexpr std::span<FFTBin const> bins() const noexcept
    {
        return bins_;
    }
};

class MutableFFTBlock {
    std::span<FFTBin> bins_ {};

public:
    constexpr MutableFFTBlock() noexcept = default;
    constexpr explicit MutableFFTBlock(std::span<FFTBin> bins) noexcept
        : bins_(bins)
    {}

    [[nodiscard]] constexpr FFTBin* data() const noexcept
    {
        return bins_.data();
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept
    {
        return bins_.size();
    }

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return bins_.empty();
    }

    [[nodiscard]] constexpr FFTBin& operator[](
        std::size_t index) const noexcept
    {
        return bins_[index];
    }

    [[nodiscard]] constexpr auto begin() const noexcept
    {
        return bins_.begin();
    }

    [[nodiscard]] constexpr auto end() const noexcept
    {
        return bins_.end();
    }

    [[nodiscard]] constexpr std::span<FFTBin> bins() const noexcept
    {
        return bins_;
    }

    [[nodiscard]] constexpr operator FFTBlock() const noexcept
    {
        return FFTBlock{bins_};
    }
};

static_assert(std::is_trivially_copyable_v<FFTBlock>);
static_assert(std::is_trivially_copyable_v<MutableFFTBlock>);

enum class ContinuousValueChannelCapability : std::uint8_t {
    scalar,
    audio,
};

namespace continuous_value_details {

inline constexpr ChannelLayout default_audio_channel_layout {
    .channel_type = ChannelTypeId::mono,
    .sample_layout = SampleStreamLayout::planar,
};

} // namespace continuous_value_details

// A registered value extension owns every type-dependent fact used by generic
// realization code. Storage elements must remain trivial: generated execution
// may copy, clear, persist and relocate them without invoking C++ object code.
struct SampleContinuousValueExtension {
    using value_type = Sample;
    using storage_element = Sample;
    using const_reference = Sample;
    using mutable_reference = Sample&;
    static constexpr std::string_view name = "Sample";
    static constexpr std::size_t rank = 0;
    static constexpr auto channel_capability =
        ContinuousValueChannelCapability::audio;
    static constexpr std::size_t channel_scalar_lanes_per_element = 1;

    using combine_values_type = Sample (*)(Sample, Sample) noexcept;

    [[nodiscard]] static constexpr const_reference read_value(
        std::span<storage_element const> elements) noexcept
    {
        return elements.front();
    }

    [[nodiscard]] static constexpr mutable_reference write_value(
        std::span<storage_element> elements) noexcept
    {
        return elements.front();
    }

    static constexpr void clear_value(mutable_reference value) noexcept
    {
        value = {};
    }

    [[nodiscard]] static constexpr Sample add(
        Sample left, Sample right) noexcept
    {
        return left + right;
    }

    struct BoundaryInputOptions {
        ChannelLayout channel_layout =
            continuous_value_details::default_audio_channel_layout;
        Sample default_value = 0.0f;
        Sample min = -std::numeric_limits<Sample::storage>::infinity();
        Sample max = std::numeric_limits<Sample::storage>::infinity();

        constexpr bool operator==(BoundaryInputOptions const&) const = default;
    };

    struct SequentialInputOptions {
        ChannelLayout channel_layout =
            continuous_value_details::default_audio_channel_layout;
        Sample default_value = 0.0f;
        Sample min = -std::numeric_limits<Sample::storage>::infinity();
        Sample max = std::numeric_limits<Sample::storage>::infinity();
        Sample neutral_value = 0.0f;
        combine_values_type combine_values = &add;

        constexpr bool operator==(SequentialInputOptions const&) const = default;
    };

    [[nodiscard]] static constexpr SequentialInputOptions
    boundary_input_sequential_options(
        BoundaryInputOptions const& options) noexcept
    {
        return {
            .channel_layout = options.channel_layout,
            .default_value = options.default_value,
            .min = options.min,
            .max = options.max,
        };
    }

    struct SequentialInputMetadata {
        Sample default_value = 0.0f;
        Sample min = -std::numeric_limits<Sample::storage>::infinity();
        Sample max = std::numeric_limits<Sample::storage>::infinity();
        Sample neutral_value = 0.0f;

        constexpr bool operator==(SequentialInputMetadata const&) const = default;

        template<class Writer>
        void write_binary(Writer& writer) const
        {
            writer.pod(default_value.value);
            writer.pod(min.value);
            writer.pod(max.value);
            writer.pod(neutral_value.value);
        }

        template<class Reader>
        [[nodiscard]] static SequentialInputMetadata read_binary(
            Reader& reader)
        {
            return {
                .default_value = Sample{
                    reader.template pod<Sample::storage>()},
                .min = Sample{reader.template pod<Sample::storage>()},
                .max = Sample{reader.template pod<Sample::storage>()},
                .neutral_value = Sample{
                    reader.template pod<Sample::storage>()},
            };
        }
    };

    struct IntrospectionMetadata {
        std::optional<Sample> default_value {};
        std::optional<Sample> min {};
        std::optional<Sample> max {};

        constexpr bool operator==(
            IntrospectionMetadata const&) const = default;

        template<class Json>
        void write_json(Json& json) const
        {
            if (default_value) {
                write_value(json, "defaultValue", *default_value);
            }
            if (min) {
                write_value(json, "minValue", *min);
            }
            if (max) {
                write_value(json, "maxValue", *max);
            }
        }

    private:
        template<class Json>
        static void write_value(
            Json& json, char const* name, Sample value)
        {
            auto const number = static_cast<Sample::storage>(value);
            if (std::isnan(number)) {
                json[name] = "nan";
            } else if (std::isinf(number)) {
                json[name] = number > 0
                    ? "positiveInfinity"
                    : "negativeInfinity";
            } else {
                json[name] = number;
            }
        }
    };

    [[nodiscard]] static constexpr IntrospectionMetadata
    introspection_metadata(SequentialInputMetadata const& metadata) noexcept
    {
        return {
            .default_value = metadata.default_value,
            .min = metadata.min,
            .max = metadata.max,
        };
    }

    template<class Metadata>
    [[nodiscard]] static constexpr IntrospectionMetadata
    introspection_metadata(Metadata const&) noexcept
    {
        return {};
    }

#define IV_AUDIO_CHANNEL_OPTIONS(Name)                                   \
    struct Name {                                                        \
        ChannelLayout channel_layout =                                   \
            continuous_value_details::default_audio_channel_layout;      \
        constexpr bool operator==(Name const&) const = default;          \
    }
    IV_AUDIO_CHANNEL_OPTIONS(RegionInputOptions);
    IV_AUDIO_CHANNEL_OPTIONS(CoverageInputOptions);
    IV_AUDIO_CHANNEL_OPTIONS(TickOutputOptions);
    IV_AUDIO_CHANNEL_OPTIONS(RegionOutputOptions);
    IV_AUDIO_CHANNEL_OPTIONS(CoverageOutputOptions);
#undef IV_AUDIO_CHANNEL_OPTIONS

    struct StatelessMetadata {
        constexpr bool operator==(StatelessMetadata const&) const = default;

        template<class Writer>
        void write_binary(Writer&) const
        {}

        template<class Reader>
        [[nodiscard]] static StatelessMetadata read_binary(Reader&)
        {
            return {};
        }
    };
    using RegionInputMetadata = StatelessMetadata;
    using CoverageInputMetadata = StatelessMetadata;
    using TickOutputMetadata = StatelessMetadata;
    using RegionOutputMetadata = StatelessMetadata;
    using CoverageOutputMetadata = StatelessMetadata;

    [[nodiscard]] static constexpr SequentialInputMetadata metadata(
        SequentialInputOptions const& options) noexcept
    {
        return {
            .default_value = options.default_value,
            .min = options.min,
            .max = options.max,
            .neutral_value = options.neutral_value,
        };
    }

#define IV_STATELESS_METADATA_OVERLOAD(Options, Metadata)                \
    [[nodiscard]] static constexpr Metadata metadata(                    \
        Options const&) noexcept                                         \
    {                                                                    \
        return {};                                                       \
    }
    IV_STATELESS_METADATA_OVERLOAD(RegionInputOptions, RegionInputMetadata)
    IV_STATELESS_METADATA_OVERLOAD(CoverageInputOptions, CoverageInputMetadata)
    IV_STATELESS_METADATA_OVERLOAD(TickOutputOptions, TickOutputMetadata)
    IV_STATELESS_METADATA_OVERLOAD(RegionOutputOptions, RegionOutputMetadata)
    IV_STATELESS_METADATA_OVERLOAD(CoverageOutputOptions, CoverageOutputMetadata)
#undef IV_STATELESS_METADATA_OVERLOAD

    static constexpr bool initialize_sequential_neutral(
        SequentialInputOptions const& options,
        std::span<storage_element> destination,
        std::size_t elements_per_value) noexcept
    {
        if (elements_per_value != 1) return false;
        for (auto& value : destination) value = options.neutral_value;
        return true;
    }

    static constexpr bool initialize_sequential_disconnected(
        SequentialInputOptions const& options,
        std::span<storage_element> destination,
        std::size_t elements_per_value) noexcept
    {
        if (elements_per_value != 1) return false;
        for (auto& value : destination) value = options.default_value;
        return true;
    }

    static constexpr bool combine_sequential_values(
        SequentialInputOptions const& options,
        std::span<storage_element> destination,
        std::span<storage_element const> source,
        std::size_t elements_per_value) noexcept
    {
        if (elements_per_value != 1 || destination.size() != source.size()
            || options.combine_values == nullptr) {
            return false;
        }
        for (std::size_t index = 0; index < destination.size(); ++index) {
            destination[index] = options.combine_values(
                destination[index], source[index]);
        }
        return true;
    }

    [[nodiscard]] static constexpr bool valid_size(
        std::optional<std::size_t> size) noexcept
    {
        return !size;
    }

    [[nodiscard]] static constexpr std::optional<std::size_t> sample_support(
        std::optional<std::size_t> size) noexcept
    {
        return valid_size(size) ? std::optional<std::size_t>{1} : std::nullopt;
    }
};

struct FractionalGlobalIndexContinuousValueExtension {
    using value_type = FractionalGlobalIndex;
    using storage_element = FractionalGlobalIndex;
    using const_reference = FractionalGlobalIndex;
    using mutable_reference = FractionalGlobalIndex&;
    static constexpr std::string_view name = "FractionalGlobalIndex";
    static constexpr std::size_t rank = 0;
    static constexpr auto channel_capability =
        ContinuousValueChannelCapability::scalar;
    static constexpr std::size_t channel_scalar_lanes_per_element = 0;

    using combine_values_type = FractionalGlobalIndex (*)(
        FractionalGlobalIndex, FractionalGlobalIndex) noexcept;

    [[nodiscard]] static constexpr const_reference read_value(
        std::span<storage_element const> elements) noexcept
    {
        return elements.front();
    }

    [[nodiscard]] static constexpr mutable_reference write_value(
        std::span<storage_element> elements) noexcept
    {
        return elements.front();
    }

    static constexpr void clear_value(mutable_reference value) noexcept
    {
        value = {};
    }

    [[nodiscard]] static constexpr FractionalGlobalIndex add(
        FractionalGlobalIndex left,
        FractionalGlobalIndex right) noexcept
    {
        return left.advanced_by({
            .whole = right.whole().bits(),
            .fraction = right.fraction(),
        });
    }

    struct BoundaryInputOptions {
        FractionalGlobalIndex default_value {};

        constexpr bool operator==(BoundaryInputOptions const&) const = default;
    };

    struct SequentialInputOptions {
        FractionalGlobalIndex default_value {};
        FractionalGlobalIndex neutral_value {};
        combine_values_type combine_values = &add;

        constexpr bool operator==(SequentialInputOptions const&) const = default;
    };

    [[nodiscard]] static constexpr SequentialInputOptions
    boundary_input_sequential_options(
        BoundaryInputOptions const& options) noexcept
    {
        return {.default_value = options.default_value};
    }

    struct SequentialInputMetadata {
        FractionalGlobalIndex default_value {};
        FractionalGlobalIndex neutral_value {};

        constexpr bool operator==(SequentialInputMetadata const&) const = default;

        template<class Writer>
        void write_binary(Writer& writer) const
        {
            writer.pod(default_value.whole().bits());
            writer.pod(default_value.fraction());
            writer.pod(neutral_value.whole().bits());
            writer.pod(neutral_value.fraction());
        }

        template<class Reader>
        [[nodiscard]] static SequentialInputMetadata read_binary(
            Reader& reader)
        {
            auto const default_whole = reader.template pod<std::uint64_t>();
            auto const default_fraction = reader.template pod<std::uint64_t>();
            auto const neutral_whole = reader.template pod<std::uint64_t>();
            auto const neutral_fraction = reader.template pod<std::uint64_t>();
            return {
                .default_value = FractionalGlobalIndex::from_parts(
                    GlobalIndex::from_bits(default_whole), default_fraction),
                .neutral_value = FractionalGlobalIndex::from_parts(
                    GlobalIndex::from_bits(neutral_whole), neutral_fraction),
            };
        }
    };

    struct IntrospectionMetadata {
        std::optional<FractionalGlobalIndex> default_value {};

        constexpr bool operator==(
            IntrospectionMetadata const&) const = default;

        template<class Json>
        void write_json(Json& json) const
        {
            if (!default_value) return;
            json["defaultValue"] = {
                {"wholeBits", std::to_string(default_value->whole().bits())},
                {"fractionBits", std::to_string(default_value->fraction())},
            };
        }
    };

    [[nodiscard]] static constexpr IntrospectionMetadata
    introspection_metadata(SequentialInputMetadata const& metadata) noexcept
    {
        return {.default_value = metadata.default_value};
    }

    template<class Metadata>
    [[nodiscard]] static constexpr IntrospectionMetadata
    introspection_metadata(Metadata const&) noexcept
    {
        return {};
    }

    struct RegionInputOptions {
        constexpr bool operator==(RegionInputOptions const&) const = default;

        template<class Writer>
        void write_binary(Writer&) const
        {}

        template<class Reader>
        [[nodiscard]] static RegionInputOptions read_binary(Reader&)
        {
            return {};
        }
    };
    using RegionInputMetadata = RegionInputOptions;
    using CoverageInputOptions = RegionInputOptions;
    using CoverageInputMetadata = RegionInputMetadata;
    struct TickOutputOptions {
        constexpr bool operator==(TickOutputOptions const&) const = default;

        template<class Writer>
        void write_binary(Writer&) const
        {}

        template<class Reader>
        [[nodiscard]] static TickOutputOptions read_binary(Reader&)
        {
            return {};
        }
    };
    using TickOutputMetadata = TickOutputOptions;
    using RegionOutputOptions = TickOutputOptions;
    using RegionOutputMetadata = TickOutputMetadata;
    using CoverageOutputOptions = TickOutputOptions;
    using CoverageOutputMetadata = TickOutputMetadata;

    [[nodiscard]] static constexpr SequentialInputMetadata metadata(
        SequentialInputOptions const& options) noexcept
    {
        return {
            .default_value = options.default_value,
            .neutral_value = options.neutral_value,
        };
    }

    static constexpr bool initialize_sequential_neutral(
        SequentialInputOptions const& options,
        std::span<storage_element> destination,
        std::size_t elements_per_value) noexcept
    {
        if (elements_per_value != 1) return false;
        for (auto& value : destination) value = options.neutral_value;
        return true;
    }

    static constexpr bool initialize_sequential_disconnected(
        SequentialInputOptions const& options,
        std::span<storage_element> destination,
        std::size_t elements_per_value) noexcept
    {
        if (elements_per_value != 1) return false;
        for (auto& value : destination) value = options.default_value;
        return true;
    }

    static constexpr bool combine_sequential_values(
        SequentialInputOptions const& options,
        std::span<storage_element> destination,
        std::span<storage_element const> source,
        std::size_t elements_per_value) noexcept
    {
        if (elements_per_value != 1 || destination.size() != source.size()
            || options.combine_values == nullptr) {
            return false;
        }
        for (std::size_t index = 0; index < destination.size(); ++index) {
            destination[index] = options.combine_values(
                destination[index], source[index]);
        }
        return true;
    }

    template<class Options>
    [[nodiscard]] static constexpr Options metadata(
        Options const& options) noexcept
    {
        return options;
    }

    [[nodiscard]] static constexpr bool valid_size(
        std::optional<std::size_t> size) noexcept
    {
        return !size;
    }

    [[nodiscard]] static constexpr std::optional<std::size_t> sample_support(
        std::optional<std::size_t>) noexcept
    {
        return std::nullopt;
    }
};

struct FFTBlockContinuousValueExtension {
    using value_type = FFTBlock;
    using storage_element = FFTBin;
    using const_reference = FFTBlock;
    using mutable_reference = MutableFFTBlock;
    static constexpr std::string_view name = "FFTBlock";
    static constexpr std::size_t rank = 1;
    static constexpr auto channel_capability =
        ContinuousValueChannelCapability::audio;
    static constexpr std::size_t channel_scalar_lanes_per_element = 2;

    using combine_values_type = void (*)(
        std::span<FFTBin>, std::span<FFTBin const>) noexcept;
    using neutral_value_type = void (*)(std::span<FFTBin>) noexcept;

    [[nodiscard]] static constexpr const_reference read_value(
        std::span<storage_element const> elements) noexcept
    {
        return FFTBlock{elements};
    }

    [[nodiscard]] static constexpr mutable_reference write_value(
        std::span<storage_element> elements) noexcept
    {
        return MutableFFTBlock{elements};
    }

    static constexpr void clear_value(mutable_reference value) noexcept
    {
        for (auto& bin : value.bins()) bin = {};
    }

    static constexpr void initialize_zero(
        std::span<FFTBin> destination) noexcept
    {
        for (auto& bin : destination) bin = {};
    }

    static constexpr void add(
        std::span<FFTBin> destination,
        std::span<FFTBin const> source) noexcept
    {
        for (std::size_t index = 0; index < destination.size(); ++index) {
            destination[index] += source[index];
        }
    }

    struct BoundaryInputOptions {
        ChannelLayout channel_layout =
            continuous_value_details::default_audio_channel_layout;

        constexpr bool operator==(BoundaryInputOptions const&) const = default;
    };

    struct SequentialInputOptions {
        ChannelLayout channel_layout =
            continuous_value_details::default_audio_channel_layout;
        neutral_value_type neutral_value = &initialize_zero;
        combine_values_type combine_values = &add;

        constexpr bool operator==(SequentialInputOptions const&) const = default;
    };

    [[nodiscard]] static constexpr SequentialInputOptions
    boundary_input_sequential_options(
        BoundaryInputOptions const& options) noexcept
    {
        return {.channel_layout = options.channel_layout};
    }

    struct SequentialInputMetadata {
        // Neutral initialization and combination are executable package
        // behavior. The configured graph has no FFT reduction payload to
        // serialize once scalar presentation metadata is excluded.
        constexpr bool operator==(SequentialInputMetadata const&) const = default;

        template<class Writer>
        void write_binary(Writer&) const
        {}

        template<class Reader>
        [[nodiscard]] static SequentialInputMetadata read_binary(Reader&)
        {
            return {};
        }
    };

    struct IntrospectionMetadata {
        constexpr bool operator==(
            IntrospectionMetadata const&) const = default;

        template<class Json>
        void write_json(Json&) const
        {}
    };

    template<class Metadata>
    [[nodiscard]] static constexpr IntrospectionMetadata
    introspection_metadata(Metadata const&) noexcept
    {
        return {};
    }

#define IV_AUDIO_CHANNEL_OPTIONS(Name)                                   \
    struct Name {                                                        \
        ChannelLayout channel_layout =                                   \
            continuous_value_details::default_audio_channel_layout;      \
        constexpr bool operator==(Name const&) const = default;          \
    }
    IV_AUDIO_CHANNEL_OPTIONS(RegionInputOptions);
    IV_AUDIO_CHANNEL_OPTIONS(CoverageInputOptions);
    IV_AUDIO_CHANNEL_OPTIONS(TickOutputOptions);
    IV_AUDIO_CHANNEL_OPTIONS(RegionOutputOptions);
    IV_AUDIO_CHANNEL_OPTIONS(CoverageOutputOptions);
#undef IV_AUDIO_CHANNEL_OPTIONS

    struct StatelessMetadata {
        constexpr bool operator==(StatelessMetadata const&) const = default;

        template<class Writer>
        void write_binary(Writer&) const
        {}

        template<class Reader>
        [[nodiscard]] static StatelessMetadata read_binary(Reader&)
        {
            return {};
        }
    };
    using RegionInputMetadata = StatelessMetadata;
    using CoverageInputMetadata = StatelessMetadata;
    using TickOutputMetadata = StatelessMetadata;
    using RegionOutputMetadata = StatelessMetadata;
    using CoverageOutputMetadata = StatelessMetadata;

    [[nodiscard]] static constexpr SequentialInputMetadata metadata(
        SequentialInputOptions const&) noexcept
    {
        return {};
    }

#define IV_STATELESS_METADATA_OVERLOAD(Options, Metadata)                \
    [[nodiscard]] static constexpr Metadata metadata(                    \
        Options const&) noexcept                                         \
    {                                                                    \
        return {};                                                       \
    }
    IV_STATELESS_METADATA_OVERLOAD(RegionInputOptions, RegionInputMetadata)
    IV_STATELESS_METADATA_OVERLOAD(CoverageInputOptions, CoverageInputMetadata)
    IV_STATELESS_METADATA_OVERLOAD(TickOutputOptions, TickOutputMetadata)
    IV_STATELESS_METADATA_OVERLOAD(RegionOutputOptions, RegionOutputMetadata)
    IV_STATELESS_METADATA_OVERLOAD(CoverageOutputOptions, CoverageOutputMetadata)
#undef IV_STATELESS_METADATA_OVERLOAD

    static constexpr bool initialize_sequential_neutral(
        SequentialInputOptions const& options,
        std::span<storage_element> destination,
        std::size_t elements_per_value) noexcept
    {
        if (!valid_size(std::optional{elements_per_value})
            || destination.size() % elements_per_value != 0
            || options.neutral_value == nullptr) {
            return false;
        }
        for (std::size_t begin = 0; begin < destination.size();
             begin += elements_per_value) {
            options.neutral_value(
                destination.subspan(begin, elements_per_value));
        }
        return true;
    }

    static constexpr bool initialize_sequential_disconnected(
        SequentialInputOptions const& options,
        std::span<storage_element> destination,
        std::size_t elements_per_value) noexcept
    {
        return initialize_sequential_neutral(
            options, destination, elements_per_value);
    }

    static constexpr bool combine_sequential_values(
        SequentialInputOptions const& options,
        std::span<storage_element> destination,
        std::span<storage_element const> source,
        std::size_t elements_per_value) noexcept
    {
        if (!valid_size(std::optional{elements_per_value})
            || destination.size() != source.size()
            || destination.size() % elements_per_value != 0
            || options.combine_values == nullptr) {
            return false;
        }
        for (std::size_t begin = 0; begin < destination.size();
             begin += elements_per_value) {
            options.combine_values(
                destination.subspan(begin, elements_per_value),
                source.subspan(begin, elements_per_value));
        }
        return true;
    }

    // The canonical real-transform half-spectrum contains N/2 + 1 explicit
    // complex bins, including DC and Nyquist. A useful transform therefore has
    // at least two bins and represents 2 * (bin_count - 1) real samples.
    [[nodiscard]] static constexpr bool valid_size(
        std::optional<std::size_t> bin_count) noexcept
    {
        return bin_count && *bin_count >= 2;
    }

    [[nodiscard]] static constexpr std::optional<std::size_t> sample_support(
        std::optional<std::size_t> bin_count) noexcept
    {
        if (!valid_size(bin_count)
            || *bin_count - 1 > std::numeric_limits<std::size_t>::max() / 2) {
            return std::nullopt;
        }
        return 2 * (*bin_count - 1);
    }
};

// This is the sole registry declaration list. A new entry names one complete
// extension; consumers must not grow type-specific switches.
#define IV_CONTINUOUS_VALUE_TYPES(X)                                      \
    X(sample, SampleContinuousValueExtension)                            \
    X(fractional_global_index,                                           \
        FractionalGlobalIndexContinuousValueExtension)                  \
    X(fft_block, FFTBlockContinuousValueExtension)

namespace continuous_value_details {

template<class... Extensions>
struct ExtensionList {};

#define IV_CONTINUOUS_EXTENSION_LIST_ENTRY(key, extension) , extension
using RegisteredExtensions = ExtensionList<
    void IV_CONTINUOUS_VALUE_TYPES(IV_CONTINUOUS_EXTENSION_LIST_ENTRY)>;
#undef IV_CONTINUOUS_EXTENSION_LIST_ENTRY

template<class List>
struct FirstRegisteredExtension;

template<class Ignored, class First, class... Rest>
struct FirstRegisteredExtension<ExtensionList<Ignored, First, Rest...>> {
    using type = First;
};

using DefaultContinuousValueExtension =
    typename FirstRegisteredExtension<RegisteredExtensions>::type;

} // namespace continuous_value_details

// Omitting a value type in authored APIs means the first extension in the sole
// registry. Sample deliberately occupies that position so the concise default
// remains the ordinary audio/control value type.
using DefaultContinuousValueType =
    typename continuous_value_details::DefaultContinuousValueExtension::value_type;
static_assert(std::same_as<DefaultContinuousValueType, Sample>,
    "the first continuous-value extension must remain Sample");

enum class ContinuousValueTypeId : std::uint8_t {
#define IV_CONTINUOUS_VALUE_TYPE_ID(key, extension) key,
    IV_CONTINUOUS_VALUE_TYPES(IV_CONTINUOUS_VALUE_TYPE_ID)
#undef IV_CONTINUOUS_VALUE_TYPE_ID
    count,
};

struct ContinuousValueTypeInfo {
    ContinuousValueTypeId id {};
    std::string_view name {};
    std::size_t rank = 0;
    ContinuousValueChannelCapability channel_capability =
        ContinuousValueChannelCapability::scalar;
    std::size_t storage_element_size = 0;
    std::size_t storage_element_alignment = 1;
    std::size_t channel_scalar_lanes_per_element = 0;
    bool (*valid_size)(std::optional<std::size_t>) noexcept = nullptr;
    std::optional<std::size_t> (*sample_support)(
        std::optional<std::size_t>) noexcept = nullptr;
    // Optional semantic conversion of one transported value across resolved
    // shapes of this same registered type. A null operation explicitly means
    // that a shape change starts fresh Sequential state.
    bool (*migrate_value_shape)(
        void* destination,
        std::size_t destination_elements,
        void const* source,
        std::size_t source_elements) noexcept = nullptr;
};

namespace continuous_value_details {

template<class Extension>
consteval auto shape_migration_operation() noexcept
{
    if constexpr (requires {
        static_cast<bool (*)(
            void*, std::size_t, void const*, std::size_t) noexcept>(
                &Extension::migrate_value_shape);
    }) {
        return static_cast<bool (*)(
            void*, std::size_t, void const*, std::size_t) noexcept>(
                &Extension::migrate_value_shape);
    } else {
        return static_cast<bool (*)(
            void*, std::size_t, void const*, std::size_t) noexcept>(nullptr);
    }
}

} // namespace continuous_value_details

inline constexpr auto continuous_value_type_registry = std::array {
#define IV_CONTINUOUS_VALUE_TYPE_INFO(key, extension)                    \
    ContinuousValueTypeInfo {                                            \
        .id = ContinuousValueTypeId::key,                                \
        .name = extension::name,                                         \
        .rank = extension::rank,                                         \
        .channel_capability = extension::channel_capability,             \
        .storage_element_size = sizeof(extension::storage_element),      \
        .storage_element_alignment = alignof(extension::storage_element),\
        .channel_scalar_lanes_per_element =                       \
            extension::channel_scalar_lanes_per_element,          \
        .valid_size = &extension::valid_size,                            \
        .sample_support = &extension::sample_support,                    \
        .migrate_value_shape =                                           \
            continuous_value_details::shape_migration_operation<         \
                extension>(),                                            \
    },
    IV_CONTINUOUS_VALUE_TYPES(IV_CONTINUOUS_VALUE_TYPE_INFO)
#undef IV_CONTINUOUS_VALUE_TYPE_INFO
};

namespace continuous_value_details {

template<class Value>
concept HasDefaultMember = requires(Value const& value) {
    value.default_value;
};

template<class Value>
concept HasMinimumMember = requires(Value const& value) {
    value.min;
};

template<class Value>
concept HasMaximumMember = requires(Value const& value) {
    value.max;
};

template<class Extension>
concept HasSequentialReduction = requires(
    typename Extension::SequentialInputOptions const& options) {
    options.neutral_value;
    options.combine_values;
};

template<class Extension>
concept HasBoundaryInputOptions = requires(
    typename Extension::BoundaryInputOptions const& options) {
    {
        Extension::boundary_input_sequential_options(options)
    } -> std::same_as<typename Extension::SequentialInputOptions>;
};

template<class Extension>
consteval bool valid_metadata_shape()
{
    if constexpr (Extension::rank != 0) {
        using Options = typename Extension::SequentialInputOptions;
        using Metadata = typename Extension::SequentialInputMetadata;
        return !HasDefaultMember<Options>
            && !HasMinimumMember<Options>
            && !HasMaximumMember<Options>
            && !HasDefaultMember<Metadata>
            && !HasMinimumMember<Metadata>
            && !HasMaximumMember<Metadata>;
    }
    return true;
}

template<class Extension>
consteval bool valid_channel_algebra_shape()
{
    if constexpr (Extension::channel_capability
        == ContinuousValueChannelCapability::audio) {
        return Extension::channel_scalar_lanes_per_element != 0
            && sizeof(typename Extension::storage_element)
                == Extension::channel_scalar_lanes_per_element * sizeof(float)
            && alignof(typename Extension::storage_element) >= alignof(float);
    }
    return Extension::channel_scalar_lanes_per_element == 0;
}

} // namespace continuous_value_details

#define IV_ASSERT_CONTINUOUS_METADATA_SHAPE(key, extension)              \
    static_assert(continuous_value_details::valid_metadata_shape<         \
        extension>(),                                                    \
        "non-scalar continuous values cannot define default/min/max"); \
    static_assert(continuous_value_details::HasSequentialReduction<       \
        extension>,                                                      \
        "every continuous value must define neutral/combine options");  \
    static_assert(continuous_value_details::HasBoundaryInputOptions<      \
        extension>,                                                      \
        "every continuous value must define boundary input options");   \
    static_assert(continuous_value_details::valid_channel_algebra_shape< \
        extension>(),                                                    \
        "audio continuous values require tightly packed float channel lanes");
IV_CONTINUOUS_VALUE_TYPES(IV_ASSERT_CONTINUOUS_METADATA_SHAPE)
#undef IV_ASSERT_CONTINUOUS_METADATA_SHAPE

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
static_assert(sizeof(FFTBin) == 2 * sizeof(Sample::storage));
static_assert(alignof(FFTBin) >= alignof(Sample::storage));

#define IV_ASSERT_CONTINUOUS_STORAGE_IS_TRIVIAL(key, extension)           \
    static_assert(std::is_trivially_copyable_v<extension::storage_element>);
IV_CONTINUOUS_VALUE_TYPES(IV_ASSERT_CONTINUOUS_STORAGE_IS_TRIVIAL)
#undef IV_ASSERT_CONTINUOUS_STORAGE_IS_TRIVIAL

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

// Complete byte geometry of one resolved transported value. A scalar has one
// storage element; a rank-one value has exactly its resolved size elements.
// Channel layout is deliberately absent: it describes how several values are
// arranged, while this record describes one value of one registered type.
struct ContinuousValueStorageGeometry {
    ContinuousValueTypeId value_type = ContinuousValueTypeId::sample;
    std::size_t elements_per_value = 1;
    std::size_t element_size = sizeof(Sample);
    std::size_t element_alignment = alignof(Sample);
    std::size_t value_size_bytes = sizeof(Sample);
    std::size_t channel_scalar_lane_count = 1;
    ContinuousValueChannelCapability channel_capability =
        ContinuousValueChannelCapability::audio;

    constexpr bool operator==(
        ContinuousValueStorageGeometry const&) const = default;
};

[[nodiscard]] constexpr std::optional<ContinuousValueStorageGeometry>
continuous_value_storage_geometry(
    ContinuousValueTypeId value_type,
    std::optional<std::size_t> resolved_size) noexcept
{
    if (!is_registered_continuous_value_type(value_type)) return std::nullopt;
    auto const& info = continuous_value_type_registry[
        static_cast<std::size_t>(value_type)];
    if (!info.valid_size || !info.valid_size(resolved_size)
        || info.storage_element_size == 0
        || info.storage_element_alignment == 0
        || !std::has_single_bit(info.storage_element_alignment)) {
        return std::nullopt;
    }
    auto const elements = info.rank == 0
        ? std::size_t{1}
        : resolved_size.value_or(0);
    if (elements == 0
        || elements > std::numeric_limits<std::size_t>::max()
                / info.storage_element_size
        || (info.channel_scalar_lanes_per_element != 0
            && elements > std::numeric_limits<std::size_t>::max()
                / info.channel_scalar_lanes_per_element)) {
        return std::nullopt;
    }
    return ContinuousValueStorageGeometry{
        .value_type = value_type,
        .elements_per_value = elements,
        .element_size = info.storage_element_size,
        .element_alignment = info.storage_element_alignment,
        .value_size_bytes = elements * info.storage_element_size,
        .channel_scalar_lane_count =
            elements * info.channel_scalar_lanes_per_element,
        .channel_capability = info.channel_capability,
    };
}

template<class T>
struct ContinuousValueType;

#define IV_DEFINE_CONTINUOUS_VALUE_TYPE(key, extension)                  \
    template<>                                                           \
    struct ContinuousValueType<typename extension::value_type>           \
        : extension {                                                    \
        static constexpr auto id = ContinuousValueTypeId::key;           \
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

// Reflection erases a leaf node's heterogeneous declaration only after the
// declaration has been statically checked. These variants are generated from
// the same sole registry list as ContinuousValueTypeId, so registering a value
// type never requires editing a second central alternative list. There is no
// empty alternative: every metadata value identifies one registered type.
namespace continuous_value_details {

template<class List>
struct MetadataVariants;

template<class Ignored, class... Extensions>
struct MetadataVariants<ExtensionList<Ignored, Extensions...>> {
    using SequentialInput = std::variant<
        typename Extensions::SequentialInputMetadata...>;
    using RegionInput = std::variant<
        typename Extensions::RegionInputMetadata...>;
    using CoverageInput = std::variant<
        typename Extensions::CoverageInputMetadata...>;
    using TickOutput = std::variant<
        typename Extensions::TickOutputMetadata...>;
    using RegionOutput = std::variant<
        typename Extensions::RegionOutputMetadata...>;
    using CoverageOutput = std::variant<
        typename Extensions::CoverageOutputMetadata...>;
    using Introspection = std::variant<
        typename Extensions::IntrospectionMetadata...>;
    using ExtensionTuple = std::tuple<Extensions...>;
};

using RegisteredMetadataVariants = MetadataVariants<RegisteredExtensions>;

} // namespace continuous_value_details

using ContinuousSequentialInputMetadata =
    continuous_value_details::RegisteredMetadataVariants::SequentialInput;
using ContinuousRegionInputMetadata =
    continuous_value_details::RegisteredMetadataVariants::RegionInput;
using ContinuousCoverageInputMetadata =
    continuous_value_details::RegisteredMetadataVariants::CoverageInput;
using ContinuousTickOutputMetadata =
    continuous_value_details::RegisteredMetadataVariants::TickOutput;
using ContinuousRegionOutputMetadata =
    continuous_value_details::RegisteredMetadataVariants::RegionOutput;
using ContinuousCoverageOutputMetadata =
    continuous_value_details::RegisteredMetadataVariants::CoverageOutput;
using ContinuousValueIntrospectionMetadata =
    continuous_value_details::RegisteredMetadataVariants::Introspection;

namespace continuous_value_details {

template<class Variants, class... Extensions, std::size_t... Index>
consteval bool metadata_variants_match_registry(
    ExtensionList<void, Extensions...>,
    std::index_sequence<Index...>)
{
    return ((
        std::same_as<
            std::variant_alternative_t<Index, typename Variants::SequentialInput>,
            typename Extensions::SequentialInputMetadata>
        && std::same_as<
            std::variant_alternative_t<Index, typename Variants::RegionInput>,
            typename Extensions::RegionInputMetadata>
        && std::same_as<
            std::variant_alternative_t<Index, typename Variants::CoverageInput>,
            typename Extensions::CoverageInputMetadata>
        && std::same_as<
            std::variant_alternative_t<Index, typename Variants::TickOutput>,
            typename Extensions::TickOutputMetadata>
        && std::same_as<
            std::variant_alternative_t<Index, typename Variants::RegionOutput>,
            typename Extensions::RegionOutputMetadata>
        && std::same_as<
            std::variant_alternative_t<Index, typename Variants::CoverageOutput>,
            typename Extensions::CoverageOutputMetadata>
        && std::same_as<
            std::variant_alternative_t<Index, typename Variants::Introspection>,
            typename Extensions::IntrospectionMetadata>
        && ContinuousValueType<typename Extensions::value_type>::id
            == continuous_value_type_registry[Index].id) && ...);
}

} // namespace continuous_value_details

static_assert(continuous_value_details::metadata_variants_match_registry<
    continuous_value_details::RegisteredMetadataVariants>(
        continuous_value_details::RegisteredExtensions{},
        std::make_index_sequence<
            continuous_value_type_registry.size()>{}));

static_assert(
    std::variant_size_v<ContinuousSequentialInputMetadata>
    == continuous_value_type_registry.size());
static_assert(
    std::variant_size_v<ContinuousRegionInputMetadata>
    == continuous_value_type_registry.size());
static_assert(
    std::variant_size_v<ContinuousCoverageInputMetadata>
    == continuous_value_type_registry.size());
static_assert(
    std::variant_size_v<ContinuousTickOutputMetadata>
    == continuous_value_type_registry.size());
static_assert(
    std::variant_size_v<ContinuousRegionOutputMetadata>
    == continuous_value_type_registry.size());
static_assert(
    std::variant_size_v<ContinuousCoverageOutputMetadata>
    == continuous_value_type_registry.size());
static_assert(
    std::variant_size_v<ContinuousValueIntrospectionMetadata>
    == continuous_value_type_registry.size());

template<class Variant>
concept ContinuousMetadataVariant =
    std::same_as<std::remove_cvref_t<Variant>,
        ContinuousSequentialInputMetadata>
    || std::same_as<std::remove_cvref_t<Variant>,
        ContinuousRegionInputMetadata>
    || std::same_as<std::remove_cvref_t<Variant>,
        ContinuousCoverageInputMetadata>
    || std::same_as<std::remove_cvref_t<Variant>,
        ContinuousTickOutputMetadata>
    || std::same_as<std::remove_cvref_t<Variant>,
        ContinuousRegionOutputMetadata>
    || std::same_as<std::remove_cvref_t<Variant>,
        ContinuousCoverageOutputMetadata>;

template<ContinuousMetadataVariant Variant>
[[nodiscard]] constexpr ContinuousValueTypeId continuous_metadata_value_type(
    Variant const& metadata)
{
    if (metadata.valueless_by_exception()
        || metadata.index() >= continuous_value_type_registry.size()) {
        throw "continuous metadata has no registered value type";
    }
    return continuous_value_type_registry[metadata.index()].id;
}

[[nodiscard]] constexpr ContinuousValueTypeId
continuous_introspection_value_type(
    ContinuousValueIntrospectionMetadata const& metadata)
{
    if (metadata.valueless_by_exception()
        || metadata.index() >= continuous_value_type_registry.size()) {
        throw "continuous introspection metadata has no registered value type";
    }
    return continuous_value_type_registry[metadata.index()].id;
}

namespace continuous_value_details {

template<std::size_t Index = 0, ContinuousMetadataVariant Metadata>
[[nodiscard]] constexpr ContinuousValueIntrospectionMetadata
make_introspection_metadata(Metadata const& metadata)
{
    if constexpr (Index >= std::variant_size_v<Metadata>) {
        throw "continuous metadata has no registered value type";
    } else {
        if (metadata.index() == Index) {
            using Extension = std::tuple_element_t<
                Index,
                typename RegisteredMetadataVariants::ExtensionTuple>;
            return ContinuousValueIntrospectionMetadata{
                std::in_place_index<Index>,
                Extension::introspection_metadata(std::get<Index>(metadata))};
        }
        return make_introspection_metadata<Index + 1>(metadata);
    }
}

} // namespace continuous_value_details

template<ContinuousMetadataVariant Metadata>
[[nodiscard]] constexpr ContinuousValueIntrospectionMetadata
continuous_introspection_metadata(Metadata const& metadata)
{
    if (metadata.valueless_by_exception()) {
        throw "continuous metadata has no registered value type";
    }
    return continuous_value_details::make_introspection_metadata(metadata);
}

template<ContinuousMetadataVariant Metadata>
[[nodiscard]] constexpr std::vector<ContinuousValueIntrospectionMetadata>
continuous_introspection_metadata(std::vector<Metadata> const& metadata)
{
    std::vector<ContinuousValueIntrospectionMetadata> result;
    result.reserve(metadata.size());
    for (auto const& value : metadata) {
        result.push_back(continuous_introspection_metadata(value));
    }
    return result;
}

#define IV_DEFAULT_CONTINUOUS_METADATA_CASE(Kind, key, extension)        \
    case ContinuousValueTypeId::key:                                    \
        return Continuous##Kind##Metadata{                               \
            extension::metadata(extension::Kind##Options{})};

[[nodiscard]] constexpr ContinuousSequentialInputMetadata
default_sequential_input_metadata(ContinuousValueTypeId type)
{
    switch (type) {
#define IV_CASE(key, extension)                                          \
    IV_DEFAULT_CONTINUOUS_METADATA_CASE(SequentialInput, key, extension)
        IV_CONTINUOUS_VALUE_TYPES(IV_CASE)
#undef IV_CASE
    case ContinuousValueTypeId::count: break;
    }
    throw "unregistered continuous value type";
}

[[nodiscard]] constexpr ContinuousRegionInputMetadata
default_region_input_metadata(ContinuousValueTypeId type)
{
    switch (type) {
#define IV_CASE(key, extension)                                          \
    IV_DEFAULT_CONTINUOUS_METADATA_CASE(RegionInput, key, extension)
        IV_CONTINUOUS_VALUE_TYPES(IV_CASE)
#undef IV_CASE
    case ContinuousValueTypeId::count: break;
    }
    throw "unregistered continuous value type";
}

[[nodiscard]] constexpr ContinuousCoverageInputMetadata
default_coverage_input_metadata(ContinuousValueTypeId type)
{
    switch (type) {
#define IV_CASE(key, extension)                                          \
    IV_DEFAULT_CONTINUOUS_METADATA_CASE(CoverageInput, key, extension)
        IV_CONTINUOUS_VALUE_TYPES(IV_CASE)
#undef IV_CASE
    case ContinuousValueTypeId::count: break;
    }
    throw "unregistered continuous value type";
}

[[nodiscard]] constexpr ContinuousTickOutputMetadata
default_tick_output_metadata(ContinuousValueTypeId type)
{
    switch (type) {
#define IV_CASE(key, extension)                                          \
    IV_DEFAULT_CONTINUOUS_METADATA_CASE(TickOutput, key, extension)
        IV_CONTINUOUS_VALUE_TYPES(IV_CASE)
#undef IV_CASE
    case ContinuousValueTypeId::count: break;
    }
    throw "unregistered continuous value type";
}

[[nodiscard]] constexpr ContinuousRegionOutputMetadata
default_region_output_metadata(ContinuousValueTypeId type)
{
    switch (type) {
#define IV_CASE(key, extension)                                          \
    IV_DEFAULT_CONTINUOUS_METADATA_CASE(RegionOutput, key, extension)
        IV_CONTINUOUS_VALUE_TYPES(IV_CASE)
#undef IV_CASE
    case ContinuousValueTypeId::count: break;
    }
    throw "unregistered continuous value type";
}

[[nodiscard]] constexpr ContinuousCoverageOutputMetadata
default_coverage_output_metadata(ContinuousValueTypeId type)
{
    switch (type) {
#define IV_CASE(key, extension)                                          \
    IV_DEFAULT_CONTINUOUS_METADATA_CASE(CoverageOutput, key, extension)
        IV_CONTINUOUS_VALUE_TYPES(IV_CASE)
#undef IV_CASE
    case ContinuousValueTypeId::count: break;
    }
    throw "unregistered continuous value type";
}

#undef IV_DEFAULT_CONTINUOUS_METADATA_CASE

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

    constexpr ContinuousValueTypeSet& operator|=(
        ContinuousValueTypeSet other) noexcept
    {
        mask_ |= other.mask_;
        return *this;
    }

    friend constexpr ContinuousValueTypeSet operator|(
        ContinuousValueTypeSet lhs,
        ContinuousValueTypeSet rhs) noexcept
    {
        lhs |= rhs;
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
