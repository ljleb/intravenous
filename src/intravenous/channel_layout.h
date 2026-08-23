#pragma once

#include <intravenous/channel_type.h>
#include <intravenous/compat.h>
#include <intravenous/sample.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace iv {
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

    template<class... Types>
    constexpr size_t channel_count(ChannelTypeId type, ChannelTypeList<Types...>)
    {
        size_t count = 0;
        auto const found = ((type == ChannelTypeTraits<Types>::id
                ? (count = Types::channel_count, true)
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
            if constexpr (SrcType == DstType && SrcLayout == DstLayout) {
                std::copy_n(src, frames * channel_count(SrcType), dst);
            } else if constexpr (
                SrcType == ChannelTypeId::mono &&
                DstType == ChannelTypeId::mono) {
                // A mono block has the same representation under both layout
                // tags, so its entire storage is contiguous.
                std::copy_n(src, frames, dst);
            } else if constexpr (
                SrcType == ChannelTypeId::mono &&
                DstType == ChannelTypeId::stereo &&
                DstLayout == SampleStreamLayout::planar) {
                // Read the source once as a contiguous block and write each
                // destination channel contiguously.
                std::copy_n(src, frames, dst);
                std::copy_n(src, frames, dst + frames);
            } else if constexpr (
                SrcType == ChannelTypeId::mono &&
                DstType == ChannelTypeId::stereo) {
                for (size_t frame = 0; frame < frames; ++frame) {
                    auto const value = src[frame];
                    dst[frame * 2] = value;
                    dst[frame * 2 + 1] = value;
                }
            } else if constexpr (
                SrcType == ChannelTypeId::stereo &&
                DstType == ChannelTypeId::mono &&
                SrcLayout == SampleStreamLayout::planar) {
                auto const* left = src;
                auto const* right = src + frames;
                for (size_t frame = 0; frame < frames; ++frame)
                    dst[frame] = (left[frame] + right[frame]) * 0.5f;
            } else if constexpr (
                SrcType == ChannelTypeId::stereo &&
                DstType == ChannelTypeId::mono) {
                for (size_t frame = 0; frame < frames; ++frame)
                    dst[frame] =
                        (src[frame * 2] + src[frame * 2 + 1]) * 0.5f;
            } else if constexpr (
                SrcType == ChannelTypeId::stereo &&
                DstType == ChannelTypeId::stereo &&
                SrcLayout == SampleStreamLayout::planar) {
                auto const* left = src;
                auto const* right = src + frames;
                for (size_t frame = 0; frame < frames; ++frame) {
                    dst[frame * 2] = left[frame];
                    dst[frame * 2 + 1] = right[frame];
                }
            } else if constexpr (
                SrcType == ChannelTypeId::stereo &&
                DstType == ChannelTypeId::stereo) {
                auto* left = dst;
                auto* right = dst + frames;
                for (size_t frame = 0; frame < frames; ++frame) {
                    left[frame] = src[frame * 2];
                    right[frame] = src[frame * 2 + 1];
                }
            } else {
                static_assert(
                    SrcType == ChannelTypeId::mono ||
                        SrcType == ChannelTypeId::stereo,
                    "unsupported source channel type");
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
