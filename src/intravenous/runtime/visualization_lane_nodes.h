#pragma once

#include <intravenous/lane_node/generate.h>
#include <intravenous/ports.h>
#include <intravenous/sample.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace iv {

struct RealtimeSampleBlockQueue {
    RealtimeSampleBlockQueue(ChannelLayout, size_t) {}

    void push(SampleBlockView<Sample const> block)
    {
        Sample::storage primary = 0.0f;
        Sample::storage secondary = 0.0f;
        for (size_t frame = 0; frame < block.frames(); ++frame) {
            primary = std::max(primary, std::abs(block.get(frame, 0).value));
            if (block.channels() > 1) {
                secondary = std::max(secondary, std::abs(block.get(frame, 1).value));
            }
        }
        peak_level_.store(primary, std::memory_order_release);
        secondary_peak_level_.store(secondary, std::memory_order_release);
    }

    [[nodiscard]] Sample::storage peak_level() const
    {
        return peak_level_.load(std::memory_order_acquire);
    }

    [[nodiscard]] Sample::storage secondary_peak_level() const
    {
        return secondary_peak_level_.load(std::memory_order_acquire);
    }

    void clear()
    {
        peak_level_.store(0.0f, std::memory_order_release);
        secondary_peak_level_.store(0.0f, std::memory_order_release);
    }

private:
    std::atomic<Sample::storage> peak_level_ { 0.0f };
    std::atomic<Sample::storage> secondary_peak_level_ { 0.0f };
};

struct RealtimeEventBlockQueue {
    static constexpr size_t kCapacity = 16;

    struct Slot {
        std::vector<TimedEvent> events {};
    };

    std::array<Slot, kCapacity> ring {};
    alignas(64) std::atomic<size_t> head { 0 };
    alignas(64) std::atomic<size_t> tail { 0 };

    void push(std::span<TimedEvent const> events)
    {
        auto const t = tail.load(std::memory_order_relaxed);
        auto const next = (t + 1) % kCapacity;
        if (next == head.load(std::memory_order_acquire)) {
            return;
        }
        ring[t].events.assign(events.begin(), events.end());
        tail.store(next, std::memory_order_release);
    }

    template<typename F>
    void drain(F&& callback)
    {
        while (true) {
            auto const h = head.load(std::memory_order_relaxed);
            if (h == tail.load(std::memory_order_acquire)) {
                break;
            }
            callback(std::span<TimedEvent const>(ring[h].events));
            head.store((h + 1) % kCapacity, std::memory_order_release);
        }
    }
};

struct VisualizationRealtimeSampleLane {
    RealtimeSampleBlockQueue* queue = nullptr;

    static std::array<LanePortConfig, 2> ports()
    {
        return {
            sample_input_port("source", LanePortDomain::realtime),
            sample_output_port("pass", LanePortDomain::realtime),
        };
    }

    void tick_block_realtime(RealtimeLaneTickContext<VisualizationRealtimeSampleLane>& ctx)
    {
        auto& output = std::get<RealtimeSampleLaneOutput>(ctx.out());
        if (ctx.realtime_sample_input(0).connected()) {
            auto const block = ctx.realtime_sample_input(0).block_view();
            output.write_block(block);
            if (queue != nullptr) {
                queue->push(block);
            }
        } else {
            if (queue != nullptr) {
                queue->clear();
            }
            auto const out = output.block_view();
            for (size_t frame = 0; frame < out.frames(); ++frame) {
                for (size_t channel = 0; channel < out.channels(); ++channel) {
                    out.set(frame, channel, Sample{});
                }
            }
        }
    }
};

struct VisualizationRealtimeEventLane {
    RealtimeEventBlockQueue* queue = nullptr;

    static std::array<LanePortConfig, 2> ports()
    {
        return {
            event_input_port("source", LanePortDomain::realtime),
            event_output_port("pass", LanePortDomain::realtime),
        };
    }

    void tick_block_realtime(RealtimeLaneTickContext<VisualizationRealtimeEventLane>& ctx)
    {
        auto const events = ctx.realtime_event_input(0).get_block();
        std::get<RealtimeEventLaneOutput>(ctx.out()).push_block(
            BlockView<TimedEvent const> { .first = events });
        if (queue != nullptr) {
            queue->push(events);
        }
    }
};

} // namespace iv
