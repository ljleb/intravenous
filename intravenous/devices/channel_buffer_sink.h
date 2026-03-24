#pragma once
#include "node.h"
#include <cassert>
#include <memory>
#include <vector>

namespace iv {
    struct ChannelBufferWriteTarget {
        Sample* channel = nullptr;
        size_t frames = 0;
        size_t global_start_index = 0;

        void inline begin(
            Sample* out_channel,
            size_t frame_count,
            size_t start_index
        ) {
            channel = out_channel;
            frames = frame_count;
            global_start_index = start_index;
        }

        void inline end() {
            channel = nullptr;
            frames = 0;
            global_start_index = 0;
        }

        [[nodiscard]] inline bool contains_global_index(size_t global_index) const {
            return global_index >= global_start_index
                && global_index < global_start_index + frames;
        }

        [[nodiscard]] inline size_t local_index(size_t global_index) const {
            assert(contains_global_index(global_index));
            return global_index - global_start_index;
        }

        inline void write(size_t global_index, Sample value) const {
            assert(channel != nullptr);
            assert(contains_global_index(global_index));
            channel[local_index(global_index)] += value;
        }
    };

    class ChannelBufferTarget {
        std::vector<std::unique_ptr<ChannelBufferWriteTarget>> _channels;

        void ensure_channels(size_t channel_count)
        {
            while (_channels.size() < channel_count) {
                _channels.push_back(std::make_unique<ChannelBufferWriteTarget>());
            }
        }

    public:
        explicit ChannelBufferTarget(size_t channel_count = 0)
        {
            ensure_channels(channel_count);
        }

        ChannelBufferTarget(ChannelBufferTarget&&) noexcept = default;
        ChannelBufferTarget& operator=(ChannelBufferTarget&&) noexcept = default;
        ChannelBufferTarget(ChannelBufferTarget const&) = delete;
        ChannelBufferTarget& operator=(ChannelBufferTarget const&) = delete;

        ChannelBufferWriteTarget& channel(size_t index)
        {
            ensure_channels(index + 1);
            return *_channels[index];
        }

        void inline begin(
            Sample** out_channels,
            size_t channel_count,
            size_t frame_count,
            size_t start_index
        ) {
            ensure_channels(channel_count);
            for (size_t channel_index = 0; channel_index < channel_count; ++channel_index) {
                _channels[channel_index]->begin(out_channels[channel_index], frame_count, start_index);
            }
            for (size_t channel_index = channel_count; channel_index < _channels.size(); ++channel_index) {
                _channels[channel_index]->end();
            }
        }

        void inline end() {
            for (auto& channel_target : _channels) {
                channel_target->end();
            }
        }
    };

    struct ChannelBufferSink {
        ChannelBufferWriteTarget* target = nullptr;

        inline explicit ChannelBufferSink(ChannelBufferTarget& target, size_t channel)
            : target(&target.channel(channel)) {}

        inline constexpr auto inputs() const {
            return std::array{
                InputConfig{
                    .name = "in",
                    .history = 0,
                    .default_value = 0.0f
                }
            };
        }

        inline void tick(TickState const& state) const {
            assert(target != nullptr);
            assert(target->contains_global_index(state.index));
            target->write(state.index, state.inputs[0].get());
        }
    };
}
