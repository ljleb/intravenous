#pragma once

#include <intravenous/lane_node/generate.h>
#include <intravenous/ports.h>
#include <intravenous/sample.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace iv {

struct RealtimeSampleBlockQueue {
    // This crosses the realtime/UI boundary with one scalar, not copied audio
    // blocks. The latest block peak is sufficient for a sampled level meter.
    RealtimeSampleBlockQueue(ChannelLayout, size_t) {}

    // Producer side (task runner thread) — lock-free and allocation-free.
    void push(SampleBlockView<Sample const> block)
    {
        Sample::storage peak = 0.0f;
        for (auto const sample : block.samples()) {
            peak = std::max(peak, std::abs(sample.value));
        }
        peak_level_.store(peak, std::memory_order_release);
    }

    // Consumer side (publisher thread): one scalar per visible lane per UI
    // update frame.
    [[nodiscard]] Sample::storage peak_level() const
    {
        return peak_level_.load(std::memory_order_acquire);
    }

    void clear()
    {
        peak_level_.store(0.0f, std::memory_order_release);
    }

private:
    std::atomic<Sample::storage> peak_level_ { 0.0f };
};

struct RealtimeEventBlockQueue {
    static constexpr size_t kCapacity = 16;

    struct Slot {
        std::vector<TimedEvent> events {};
    };

    std::array<Slot, kCapacity> ring {};
    alignas(64) std::atomic<size_t> head { 0 };
    alignas(64) std::atomic<size_t> tail { 0 };

    // Producer side (task runner thread) — lock-free
    void push(std::span<TimedEvent const> events)
    {
        auto const t = tail.load(std::memory_order_relaxed);
        auto const next = (t + 1) % kCapacity;
        if (next == head.load(std::memory_order_acquire)) {
            return; // full — drop
        }
        ring[t].events.assign(events.begin(), events.end());
        tail.store(next, std::memory_order_release);
    }

    // Consumer side (publisher thread) — lock-free
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
    RealtimeSampleBlockQueue* queue = nullptr; // non-owning

    static std::array<RealtimeSampleLaneInputConfig, 1> realtime_sample_inputs()
    {
        return { RealtimeSampleLaneInputConfig { .name = "source" } };
    }

    static RealtimeSampleLaneOutputConfig output()
    {
        return RealtimeSampleLaneOutputConfig { .name = "pass" };
    }

    void tick_block_realtime(RealtimeLaneTickContext<VisualizationRealtimeSampleLane>& ctx)
    {
        if (ctx.realtime_sample_input(0).connected()) {
            auto const block = ctx.realtime_sample_input(0).block_view();
            ctx.out().write_block(block);
            if (queue != nullptr) {
                queue->push(block);
            }
        } else {
            if (queue != nullptr) {
                queue->clear();
            }
            auto const out = ctx.out().block_view();
            for (size_t frame = 0; frame < out.frames(); ++frame) {
                for (size_t channel = 0; channel < out.channels(); ++channel) {
                    out.set(frame, channel, Sample{});
                }
            }
        }
    }
};

struct VisualizationRealtimeEventLane {
    RealtimeEventBlockQueue* queue = nullptr; // non-owning

    static std::array<RealtimeEventLaneInputConfig, 1> realtime_event_inputs()
    {
        return { RealtimeEventLaneInputConfig { .name = "source" } };
    }

    static RealtimeEventLaneOutputConfig output()
    {
        return RealtimeEventLaneOutputConfig { .name = "pass" };
    }

    void tick_block_realtime(RealtimeLaneTickContext<VisualizationRealtimeEventLane>& ctx)
    {
        auto const events = ctx.realtime_event_input(0).get_block();
        ctx.out().push_block(BlockView<TimedEvent const> { .first = events });
        if (queue != nullptr) {
            queue->push(events);
        }
    }
};

} // namespace iv
