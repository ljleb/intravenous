#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace iv {

enum class PlaybackPreparationPhase {
    background_pass,
    reclaim_snapshots,
    cache_maintenance,
    prepare_playback,
    transaction,
    propagate_coverage,
    prepare_evaluation,
    evaluate_tock,
    freeze_tick_inputs,
    retain_tick_inputs,
    prepare_tick_residency,
    store_output,
    replace_sample_regions,
    publish_output,
    select_foreground_payload,
    publish_foreground_snapshot,
    map_sample_view,
    mapping_vma_check,
    residency_vma_check,
    mmap_reserve,
    mmap_backing,
    munmap_call,
    mlock_call,
    munlock_call,
    read_backing_page,
    allocate_backing_page,
    write_backing_page,
    count,
};

// One background worker writes these counters. The benchmark reads them
// concurrently. A snapshot taken during work can straddle a phase completion.
class PlaybackPreparationMetrics {
public:
    struct Measurement {
        std::uint64_t calls = 0;
        std::uint64_t total_ns = 0;
        std::uint64_t max_ns = 0;
        std::uint64_t work = 0;
    };
    static constexpr std::size_t phase_count = static_cast<std::size_t>(PlaybackPreparationPhase::count);
    using Snapshot = std::array<Measurement, phase_count>;
    static constexpr std::array<std::string_view, phase_count> phase_names{
        "background_pass", "reclaim_snapshots", "cache_maintenance", "prepare_playback",
        "transaction", "propagate_coverage", "prepare_evaluation", "evaluate_tock",
        "freeze_tick_inputs", "retain_tick_inputs", "prepare_tick_residency", "store_output",
        "replace_sample_regions", "publish_output", "select_foreground_payload",
        "publish_foreground_snapshot", "map_sample_view", "mapping_vma_check", "residency_vma_check",
        "mmap_reserve", "mmap_backing", "munmap", "mlock", "munlock", "read_backing_page",
        "allocate_backing_page", "write_backing_page",
    };

private:
    struct Counters {
        std::atomic<std::uint64_t> calls{0}, total_ns{0}, max_ns{0}, work{0};
    };
    std::array<Counters, phase_count> phases_{};
    std::atomic<bool> enabled_{false};
    inline static thread_local PlaybackPreparationMetrics* active_ = nullptr;

public:
    void enable() noexcept { enabled_.store(true, std::memory_order_release); }

    class Attachment {
        PlaybackPreparationMetrics* previous_;

    public:
        explicit Attachment(PlaybackPreparationMetrics* metrics) noexcept
            : previous_(std::exchange(active_, metrics)) {}
        ~Attachment() { active_ = previous_; }
        Attachment(Attachment const&) = delete;
        Attachment& operator=(Attachment const&) = delete;
    };

    class Interval {
        using Clock = std::chrono::steady_clock;
        Counters* counters_ = nullptr;
        Clock::time_point start_{};
        std::uint64_t work_ = 0;

    public:
        explicit Interval(PlaybackPreparationPhase phase, std::uint64_t work = 0) noexcept
            : work_(work)
        {
            if (auto* active = active_; active && active->enabled_.load(std::memory_order_acquire)) {
                counters_ = &active->phases_[static_cast<std::size_t>(phase)];
                start_ = Clock::now();
            }
        }
        ~Interval()
        {
            if (!counters_) return;
            auto const elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count());
            counters_->total_ns.fetch_add(elapsed, std::memory_order_relaxed);
            counters_->work.fetch_add(work_, std::memory_order_relaxed);
            if (elapsed > counters_->max_ns.load(std::memory_order_relaxed))
                counters_->max_ns.store(elapsed, std::memory_order_relaxed);
            counters_->calls.fetch_add(1, std::memory_order_relaxed);
        }
        Interval(Interval const&) = delete;
        Interval& operator=(Interval const&) = delete;

        void set_work(std::uint64_t work) noexcept { work_ = work; }
    };

    [[nodiscard]] Snapshot snapshot() const noexcept
    {
        Snapshot result;
        for (std::size_t i = 0; i < phase_count; ++i) {
            auto const& c = phases_[i];
            result[i] = {c.calls.load(std::memory_order_relaxed), c.total_ns.load(std::memory_order_relaxed),
                c.max_ns.load(std::memory_order_relaxed), c.work.load(std::memory_order_relaxed)};
        }
        return result;
    }
};

template<class Function>
decltype(auto) measure_playback_preparation(
    PlaybackPreparationPhase phase, Function&& function, std::uint64_t work = 0)
{
    PlaybackPreparationMetrics::Interval interval{phase, work};
    return std::forward<Function>(function)();
}

} // namespace iv
