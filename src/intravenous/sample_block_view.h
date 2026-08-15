#pragma once

#include <intravenous/channel_layout.h>

#include <span>
#include <type_traits>

namespace iv {
    template<typename T>
    class SampleBlockView {
        std::span<T> _samples {};
        ChannelLayout _layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
        size_t _frame_count = 0;

        static constexpr size_t sample_offset(
            SampleStreamLayout layout,
            size_t frame,
            size_t channel,
            size_t frame_count,
            size_t channel_count_value) noexcept
        {
            return layout == SampleStreamLayout::planar
                ? channel * frame_count + frame
                : frame * channel_count_value + channel;
        }

        class Axis {
            SampleBlockView _view;
            size_t _outer = 0;

        public:
            constexpr Axis(SampleBlockView view, size_t outer) : _view(view), _outer(outer) {}

            constexpr decltype(auto) operator[](size_t inner) const
            {
                // Planar storage is channel-first; interleaved storage is frame-first.
                return _view.at(
                    _view.sample_layout() == SampleStreamLayout::planar ? inner : _outer,
                    _view.sample_layout() == SampleStreamLayout::planar ? _outer : inner
                );
            }
        };

        class ChannelAxis {
            SampleBlockView _view;
            size_t _channel = 0;

        public:
            constexpr ChannelAxis(SampleBlockView view, size_t channel) : _view(view), _channel(channel) {}
            constexpr decltype(auto) operator[](size_t frame) const { return _view.at(frame, _channel); }
        };

    public:
        using value_type = std::remove_cv_t<T>;

        SampleBlockView() = default;
        constexpr SampleBlockView(std::span<T> samples, ChannelLayout layout, size_t frame_count) :
            _samples(samples), _layout(layout), _frame_count(frame_count)
        {
            IV_ASSERT(samples.size() == sample_storage_size(layout, frame_count), "sample block storage does not match layout");
        }

        constexpr std::span<T> samples() const { return _samples; }
        constexpr ChannelLayout channel_layout() const { return _layout; }
        constexpr ChannelTypeId channel_type() const { return _layout.channel_type; }
        constexpr SampleStreamLayout sample_layout() const { return _layout.sample_layout; }
        constexpr size_t channels() const { return channel_count(_layout); }
        constexpr size_t frames() const { return _frame_count; }
        constexpr size_t size() const { return _samples.size(); }
        constexpr bool empty() const { return _samples.empty(); }

        constexpr decltype(auto) at(size_t frame, size_t channel) const
        {
            IV_ASSERT(frame < frames() && channel < channels(), "sample block index out of bounds");
            return _samples[sample_offset(_layout.sample_layout, frame, channel, frames(), channels())];
        }

        constexpr decltype(auto) get(size_t frame, size_t channel) const { return at(frame, channel); }
        template<typename U = T>
        requires (!std::is_const_v<U>)
        constexpr void set(size_t frame, size_t channel, Sample value) const { at(frame, channel) = value; }

        // `view[channel][frame]` for planar storage; `view[frame][channel]`
        // for interleaved storage. Both expose the declared representation.
        constexpr Axis operator[](size_t outer) const { return Axis(*this, outer); }

        constexpr ChannelAxis channel(size_t channel) const
        {
            IV_ASSERT(channel < channels(), "channel index out of bounds");
            // A named/channel-oriented access path is independent of storage.
            return ChannelAxis(*this, channel);
        }

        constexpr ChannelAxis left() const
        {
            IV_ASSERT(channels() == 2, "left() requires a two-channel sample block");
            return channel(0);
        }

        constexpr ChannelAxis right() const
        {
            IV_ASSERT(channels() == 2, "right() requires a two-channel sample block");
            return channel(1);
        }

        constexpr std::span<T> channel_span(size_t channel) const
        {
            if (sample_layout() != SampleStreamLayout::planar || channel >= channels()) {
                return {};
            }
            return _samples.subspan(channel * frames(), frames());
        }

        constexpr T* interleaved_frame_ptr(size_t frame) const
        {
            if (sample_layout() != SampleStreamLayout::interleaved || frame >= frames()) {
                return nullptr;
            }
            return _samples.data() + frame * channels();
        }
    };
}
