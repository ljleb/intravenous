#pragma once
#include "miniaudio/miniaudio.h"

#include "node.h"
#include "graph_node.h"

#include <iostream>
#include <memory>
#include <vector>
#include <cstring>
#include <cstddef>
#include <cassert>
#include <array>


namespace iv {
    struct RealtimeOutputTarget {
        Sample** channels = nullptr;
        size_t num_channels = 0;
        size_t frames = 0;
        size_t global_start_index = 0;

        void inline begin(
            Sample** out_channels,
            size_t channel_count,
            size_t frame_count,
            size_t start_index
        ) {
            channels = out_channels;
            num_channels = channel_count;
            frames = frame_count;
            global_start_index = start_index;
        }

        void inline end() {
            channels = nullptr;
            num_channels = 0;
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

        inline void write(size_t channel, size_t global_index, Sample value) const {
            assert(channels != nullptr);
            assert(channel < num_channels);
            assert(contains_global_index(global_index));
            channels[channel][local_index(global_index)] = value;
        }
    };

    struct OutputDeviceSink {
        RealtimeOutputTarget* target = nullptr;
        size_t channel = 0;

        inline explicit OutputDeviceSink(RealtimeOutputTarget& target, size_t channel)
            : target(&target), channel(channel) {}

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
            target->write(channel, state.index, state.inputs[0].get());
        }
    };
}
