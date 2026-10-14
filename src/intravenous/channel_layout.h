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
        return is_registered_channel_type(type);
    }

    constexpr bool is_valid_sample_stream_layout(SampleStreamLayout layout) noexcept
    {
        return static_cast<size_t>(layout) < static_cast<size_t>(SampleStreamLayout::count);
    }

    constexpr size_t channel_count(ChannelTypeId type)
    {
        return channel_type_info(type).channel_count;
    }

    constexpr size_t channel_count(ChannelLayout layout)
    {
        return channel_count(layout.channel_type);
    }

    template<class... Types>
    consteval size_t maximum_channel_count(ChannelTypeList<Types...>)
    {
        size_t result = 0;
        ((result = std::max(result, Types::channel_count)), ...);
        return result;
    }

    inline constexpr size_t maximum_supported_channel_count =
        maximum_channel_count(SupportedChannelTypes{});
    static_assert(maximum_supported_channel_count != 0);

    constexpr size_t sample_storage_size(ChannelLayout layout, size_t frames)
    {
        return frames * channel_count(layout);
    }

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
        template<class Type, SampleStreamLayout Layout>
        constexpr size_t sample_offset(size_t frame, size_t channel, size_t frames)
        {
            if constexpr (Layout == SampleStreamLayout::planar) {
                return channel * frames + frame;
            } else {
                return frame * Type::channel_count + channel;
            }
        }

        template<class Type, SampleStreamLayout Layout>
        IV_FORCEINLINE Sample read_sample(Sample const* src, size_t frame, size_t channel, size_t frames)
        {
            return src[sample_offset<Type, Layout>(frame, channel, frames)];
        }

        template<class Type, SampleStreamLayout Layout>
        IV_FORCEINLINE void write_sample(Sample* dst, size_t frame, size_t channel, size_t frames, Sample value)
        {
            dst[sample_offset<Type, Layout>(frame, channel, frames)] = value;
        }

        template<
            class SrcType,
            SampleStreamLayout SrcLayout,
            class DstType,
            SampleStreamLayout DstLayout>
        void convert_block(Sample const* src, Sample* dst, size_t frames)
        {
            if constexpr (std::same_as<SrcType, DstType>
                && SrcLayout == DstLayout) {
                std::copy_n(src, frames * SrcType::channel_count, dst);
            } else if constexpr (std::same_as<SrcType, DstType>
                && SrcType::channel_count == 1) {
                // One-channel planar and interleaved blocks have the same
                // representation, independently of the channel type's name.
                std::copy_n(src, frames, dst);
            } else {
                for (size_t frame = 0; frame < frames; ++frame) {
                    for (size_t target_channel = 0;
                         target_channel < DstType::channel_count;
                         ++target_channel) {
                        Sample value {};
                        for (size_t source_channel = 0;
                             source_channel < SrcType::channel_count;
                             ++source_channel) {
                            auto const coefficient =
                                ChannelConversion<SrcType, DstType>::coefficients[
                                    target_channel * SrcType::channel_count
                                    + source_channel];
                            if (coefficient == 0.0f) continue;
                            value += read_sample<SrcType, SrcLayout>(
                                src, frame, source_channel, frames)
                                * coefficient;
                        }
                        write_sample<DstType, DstLayout>(
                            dst, frame, target_channel, frames, value);
                    }
                }
            }
        }

        template<
            class SrcType,
            SampleStreamLayout SrcLayout,
            class DstType,
            SampleStreamLayout DstLayout>
        constexpr ChannelConversionPlan make_plan() noexcept
        {
            return ChannelConversionPlan {
                .source = ChannelLayout {
                    .channel_type = ChannelTypeTraits<SrcType>::id,
                    .sample_layout = SrcLayout,
                },
                .target = ChannelLayout {
                    .channel_type = ChannelTypeTraits<DstType>::id,
                    .sample_layout = DstLayout,
                },
                .convert = &convert_block<SrcType, SrcLayout, DstType, DstLayout>,
            };
        }

        template<class SrcType, class DstType>
        constexpr ChannelConversionPlan make_plan(
            SampleStreamLayout source_layout,
            SampleStreamLayout target_layout)
        {
            if (source_layout == SampleStreamLayout::planar) {
                if (target_layout == SampleStreamLayout::planar) {
                    return make_plan<SrcType,
                        SampleStreamLayout::planar,
                        DstType,
                        SampleStreamLayout::planar>();
                }
                return make_plan<SrcType,
                    SampleStreamLayout::planar,
                    DstType,
                    SampleStreamLayout::interleaved>();
            }
            if (target_layout == SampleStreamLayout::planar) {
                return make_plan<SrcType,
                    SampleStreamLayout::interleaved,
                    DstType,
                    SampleStreamLayout::planar>();
            }
            return make_plan<SrcType,
                SampleStreamLayout::interleaved,
                DstType,
                SampleStreamLayout::interleaved>();
        }

        template<class SrcType, class... DstTypes>
        constexpr ChannelConversionPlan make_plan_for_target(
            ChannelLayout source,
            ChannelLayout target,
            ChannelTypeList<DstTypes...>)
        {
            ChannelConversionPlan result {};
            auto const found = ((target.channel_type
                        == ChannelTypeTraits<DstTypes>::id
                    ? (result = make_plan<SrcType, DstTypes>(
                           source.sample_layout, target.sample_layout),
                        true)
                    : false)
                || ...);
            if (!found) throw std::logic_error("invalid target channel type");
            return result;
        }

        template<class... SrcTypes>
        constexpr ChannelConversionPlan make_plan_for_source(
            ChannelLayout source,
            ChannelLayout target,
            ChannelTypeList<SrcTypes...> types)
        {
            ChannelConversionPlan result {};
            auto const found = ((source.channel_type
                        == ChannelTypeTraits<SrcTypes>::id
                    ? (result = make_plan_for_target<SrcTypes>(
                           source, target, types),
                        true)
                    : false)
                || ...);
            if (!found) throw std::logic_error("invalid source channel type");
            return result;
        }
    } // namespace channel_details

    class ChannelConversionRegistry {
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

            return channel_details::make_plan_for_source(
                source, target, SupportedChannelTypes{});
        }
    };
} // namespace iv
