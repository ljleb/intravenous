#pragma once

#include <intravenous/lane_node/generate.h>
#include <intravenous/ports.h>
#include <intravenous/sample.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

namespace iv {

struct RealtimeSampleBlockQueue {
    static constexpr size_t kCapacity = 16;

    struct Slot {
        std::vector<Sample> samples {};
    };

    std::array<Slot, kCapacity> ring {};
    alignas(64) std::atomic<size_t> head { 0 };
    alignas(64) std::atomic<size_t> tail { 0 };

    explicit RealtimeSampleBlockQueue(size_t block_size)
    {
        for (auto& slot : ring) {
            slot.samples.resize(block_size);
        }
    }

    // Producer side (task runner thread) — lock-free
    void push(std::span<Sample const> block)
    {
        auto const t = tail.load(std::memory_order_relaxed);
        auto const next = (t + 1) % kCapacity;
        if (next == head.load(std::memory_order_acquire)) {
            return; // full — drop
        }
        auto& slot = ring[t];
        auto const n = std::min(block.size(), slot.samples.size());
        std::copy_n(block.begin(), n, slot.samples.begin());
        slot.samples.resize(n);
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
            callback(std::span<Sample const>(ring[h].samples));
            head.store((h + 1) % kCapacity, std::memory_order_release);
        }
    }
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
            auto const block = ctx.realtime_sample_input(0).get_block();
            for (size_t i = 0; i < ctx.sample_count(); ++i) {
                ctx.out().push(i < block.size() ? block[i] : Sample{});
            }
            if (queue != nullptr) {
                queue->push(block);
            }
        } else {
            for (size_t i = 0; i < ctx.sample_count(); ++i) {
                ctx.out().push(Sample{});
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
