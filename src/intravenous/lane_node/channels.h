#pragma once

#include <intravenous/ports.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace iv {
    enum class ChannelTypeId : std::uint8_t {
        mono,
        stereo,
        count,
    };

    template<ChannelTypeId Type>
    struct ChannelTypeTraits;

    template<>
    struct ChannelTypeTraits<ChannelTypeId::mono> {
        static constexpr size_t count = 1;
    };

    template<>
    struct ChannelTypeTraits<ChannelTypeId::stereo> {
        static constexpr size_t count = 2;
    };

    template<ChannelTypeId... Types>
    struct ChannelTypeList {};

    // Register each supported layout once. Consumers use this registry instead
    // of maintaining their own mono/stereo dispatch switches.
    using SupportedChannelTypes = ChannelTypeList<
        ChannelTypeId::mono,
        ChannelTypeId::stereo>;

    enum class SampleStreamLayout : std::uint8_t {
        planar,
        interleaved,
        count,
    };

    struct ChannelLayout {
        ChannelTypeId channel_type = ChannelTypeId::stereo;
        SampleStreamLayout sample_layout = SampleStreamLayout::planar;

        bool operator==(ChannelLayout const&) const = default;
    };

    constexpr bool is_valid_channel_type(ChannelTypeId type) noexcept
    {
        return static_cast<size_t>(type) < static_cast<size_t>(ChannelTypeId::count);
    }

    constexpr bool is_valid_sample_stream_layout(SampleStreamLayout layout) noexcept
    {
        return static_cast<size_t>(layout) < static_cast<size_t>(SampleStreamLayout::count);
    }

    template<ChannelTypeId... Types>
    constexpr size_t channel_count(ChannelTypeId type, ChannelTypeList<Types...>)
    {
        size_t count = 0;
        auto const found = ((type == Types
                ? (count = ChannelTypeTraits<Types>::count, true)
                : false)
            || ...);
        if (!found) {
            throw std::logic_error("invalid channel type");
        }
        return count;
    }

    constexpr size_t channel_count(ChannelTypeId type)
    {
        return channel_count(type, SupportedChannelTypes{});
    }

    constexpr size_t channel_count(ChannelLayout layout)
    {
        return channel_count(layout.channel_type);
    }

    constexpr size_t sample_storage_size(ChannelLayout layout, size_t frames)
    {
        return frames * channel_count(layout);
    }

    enum class ChannelConversionStepId : std::uint8_t {
        mono_to_mono,
        mono_to_stereo,
        stereo_to_mono,
        stereo_to_stereo,
    };

    struct ChannelConversionPlan {
        ChannelLayout source {};
        ChannelLayout target {};
        void (*convert)(Sample const* src, Sample* dst, size_t frames) = nullptr;

        explicit constexpr operator bool() const noexcept
        {
            return convert != nullptr;
        }
    };

    namespace channel_details {
        template<ChannelTypeId Type, SampleStreamLayout Layout>
        constexpr size_t sample_offset(size_t frame, size_t channel, size_t frames)
        {
            constexpr size_t channels = channel_count(Type);
            (void)channels;
            if constexpr (Layout == SampleStreamLayout::planar) {
                return channel * frames + frame;
            } else {
                return frame * channel_count(Type) + channel;
            }
        }

        template<ChannelTypeId Type, SampleStreamLayout Layout>
        IV_FORCEINLINE Sample read_sample(Sample const* src, size_t frame, size_t channel, size_t frames)
        {
            return src[sample_offset<Type, Layout>(frame, channel, frames)];
        }

        template<ChannelTypeId Type, SampleStreamLayout Layout>
        IV_FORCEINLINE void write_sample(Sample* dst, size_t frame, size_t channel, size_t frames, Sample value)
        {
            dst[sample_offset<Type, Layout>(frame, channel, frames)] = value;
        }

        template<
            ChannelTypeId SrcType,
            SampleStreamLayout SrcLayout,
            ChannelTypeId DstType,
            SampleStreamLayout DstLayout>
        void convert_block(Sample const* src, Sample* dst, size_t frames)
        {
            for (size_t frame = 0; frame < frames; ++frame) {
                Sample dst_values[2] {};

                if constexpr (SrcType == ChannelTypeId::mono && DstType == ChannelTypeId::mono) {
                    dst_values[0] = read_sample<ChannelTypeId::mono, SrcLayout>(src, frame, 0, frames);
                } else if constexpr (SrcType == ChannelTypeId::mono && DstType == ChannelTypeId::stereo) {
                    Sample const value = read_sample<ChannelTypeId::mono, SrcLayout>(src, frame, 0, frames);
                    dst_values[0] = value;
                    dst_values[1] = value;
                } else if constexpr (SrcType == ChannelTypeId::stereo && DstType == ChannelTypeId::mono) {
                    Sample const left = read_sample<ChannelTypeId::stereo, SrcLayout>(src, frame, 0, frames);
                    Sample const right = read_sample<ChannelTypeId::stereo, SrcLayout>(src, frame, 1, frames);
                    dst_values[0] = (left + right) * 0.5f;
                } else if constexpr (SrcType == ChannelTypeId::stereo && DstType == ChannelTypeId::stereo) {
                    dst_values[0] = read_sample<ChannelTypeId::stereo, SrcLayout>(src, frame, 0, frames);
                    dst_values[1] = read_sample<ChannelTypeId::stereo, SrcLayout>(src, frame, 1, frames);
                } else {
                    static_assert(
                        SrcType == ChannelTypeId::mono || SrcType == ChannelTypeId::stereo,
                        "unsupported source channel type");
                }

                if constexpr (DstType == ChannelTypeId::mono) {
                    write_sample<ChannelTypeId::mono, DstLayout>(dst, frame, 0, frames, dst_values[0]);
                } else if constexpr (DstType == ChannelTypeId::stereo) {
                    write_sample<ChannelTypeId::stereo, DstLayout>(dst, frame, 0, frames, dst_values[0]);
                    write_sample<ChannelTypeId::stereo, DstLayout>(dst, frame, 1, frames, dst_values[1]);
                } else {
                    static_assert(
                        DstType == ChannelTypeId::mono || DstType == ChannelTypeId::stereo,
                        "unsupported destination channel type");
                }
            }
        }

        template<
            ChannelTypeId SrcType,
            SampleStreamLayout SrcLayout,
            ChannelTypeId DstType,
            SampleStreamLayout DstLayout>
        constexpr ChannelConversionPlan make_plan() noexcept
        {
            return ChannelConversionPlan {
                .source = ChannelLayout {
                    .channel_type = SrcType,
                    .sample_layout = SrcLayout,
                },
                .target = ChannelLayout {
                    .channel_type = DstType,
                    .sample_layout = DstLayout,
                },
                .convert = &convert_block<SrcType, SrcLayout, DstType, DstLayout>,
            };
        }
    } // namespace channel_details

    class ChannelConversionRegistry {
        static constexpr auto plans() noexcept
        {
            return std::array {
                channel_details::make_plan<
                    ChannelTypeId::mono,
                    SampleStreamLayout::planar,
                    ChannelTypeId::mono,
                    SampleStreamLayout::planar>(),
                channel_details::make_plan<
                    ChannelTypeId::mono,
                    SampleStreamLayout::planar,
                    ChannelTypeId::mono,
                    SampleStreamLayout::interleaved>(),
                channel_details::make_plan<
                    ChannelTypeId::mono,
                    SampleStreamLayout::planar,
                    ChannelTypeId::stereo,
                    SampleStreamLayout::planar>(),
                channel_details::make_plan<
                    ChannelTypeId::mono,
                    SampleStreamLayout::planar,
                    ChannelTypeId::stereo,
                    SampleStreamLayout::interleaved>(),
                channel_details::make_plan<
                    ChannelTypeId::mono,
                    SampleStreamLayout::interleaved,
                    ChannelTypeId::mono,
                    SampleStreamLayout::planar>(),
                channel_details::make_plan<
                    ChannelTypeId::mono,
                    SampleStreamLayout::interleaved,
                    ChannelTypeId::mono,
                    SampleStreamLayout::interleaved>(),
                channel_details::make_plan<
                    ChannelTypeId::mono,
                    SampleStreamLayout::interleaved,
                    ChannelTypeId::stereo,
                    SampleStreamLayout::planar>(),
                channel_details::make_plan<
                    ChannelTypeId::mono,
                    SampleStreamLayout::interleaved,
                    ChannelTypeId::stereo,
                    SampleStreamLayout::interleaved>(),
                channel_details::make_plan<
                    ChannelTypeId::stereo,
                    SampleStreamLayout::planar,
                    ChannelTypeId::mono,
                    SampleStreamLayout::planar>(),
                channel_details::make_plan<
                    ChannelTypeId::stereo,
                    SampleStreamLayout::planar,
                    ChannelTypeId::mono,
                    SampleStreamLayout::interleaved>(),
                channel_details::make_plan<
                    ChannelTypeId::stereo,
                    SampleStreamLayout::planar,
                    ChannelTypeId::stereo,
                    SampleStreamLayout::planar>(),
                channel_details::make_plan<
                    ChannelTypeId::stereo,
                    SampleStreamLayout::planar,
                    ChannelTypeId::stereo,
                    SampleStreamLayout::interleaved>(),
                channel_details::make_plan<
                    ChannelTypeId::stereo,
                    SampleStreamLayout::interleaved,
                    ChannelTypeId::mono,
                    SampleStreamLayout::planar>(),
                channel_details::make_plan<
                    ChannelTypeId::stereo,
                    SampleStreamLayout::interleaved,
                    ChannelTypeId::mono,
                    SampleStreamLayout::interleaved>(),
                channel_details::make_plan<
                    ChannelTypeId::stereo,
                    SampleStreamLayout::interleaved,
                    ChannelTypeId::stereo,
                    SampleStreamLayout::planar>(),
                channel_details::make_plan<
                    ChannelTypeId::stereo,
                    SampleStreamLayout::interleaved,
                    ChannelTypeId::stereo,
                    SampleStreamLayout::interleaved>(),
            };
        }

    public:
        static constexpr ChannelConversionPlan plan(ChannelLayout source, ChannelLayout target)
        {
            if (!is_valid_channel_type(source.channel_type) || !is_valid_channel_type(target.channel_type)) {
                throw std::logic_error("invalid channel type");
            }
            if (!is_valid_sample_stream_layout(source.sample_layout)
                || !is_valid_sample_stream_layout(target.sample_layout)) {
                throw std::logic_error("invalid sample stream layout");
            }

            for (auto const& candidate : plans()) {
                if (candidate.source == source && candidate.target == target) {
                    return candidate;
                }
            }
            throw std::logic_error("no channel conversion path is available");
        }
    };
} // namespace iv
