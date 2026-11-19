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
    reuse_sample_window,
    resident_pool_copy,
    resident_pool_fallback,
    stable_sample_window,
    count,
};

// One background worker writes these counters. The benchmark reads them
// concurrently. A snapshot taken during work can straddle a phase completion.
class PlaybackPreparationMetrics {
public:
    enum class SamplePreparation { remap, reuse, pool, arena };

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
        "reuse_sample_window", "resident_pool_copy", "resident_pool_fallback", "stable_sample_window",
    };

private:
    struct Counters {
        std::atomic<std::uint64_t> calls{0}, total_ns{0}, max_ns{0}, work{0};
    };
    std::array<Counters, phase_count> phases_{};
    std::atomic<bool> enabled_{false};
    SamplePreparation sample_preparation_;
    static constexpr std::uint64_t latency_bucket_ns = 100'000;
    static constexpr std::size_t latency_bucket_count = 200;
    std::array<std::atomic<std::uint64_t>, latency_bucket_count> transaction_latency_{};
    inline static thread_local PlaybackPreparationMetrics* active_ = nullptr;

public:
    explicit PlaybackPreparationMetrics(SamplePreparation sample_preparation = SamplePreparation::remap) noexcept
        : sample_preparation_(sample_preparation) {}

    [[nodiscard]] static SamplePreparation active_sample_preparation() noexcept
    {
        return active_ ? active_->sample_preparation_ : SamplePreparation::remap;
    }

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
        PlaybackPreparationMetrics* transaction_metrics_ = nullptr;
        Clock::time_point start_{};
        std::uint64_t work_ = 0;

    public:
        explicit Interval(PlaybackPreparationPhase phase, std::uint64_t work = 0) noexcept
            : work_(work)
        {
            if (auto* active = active_; active && active->enabled_.load(std::memory_order_acquire)) {
                counters_ = &active->phases_[static_cast<std::size_t>(phase)];
                if (phase == PlaybackPreparationPhase::transaction) transaction_metrics_ = active;
                start_ = Clock::now();
            }
        }
        ~Interval()
        {
            if (!counters_) return;
            auto const elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count());
            if (transaction_metrics_) {
                auto& buckets = transaction_metrics_->transaction_latency_;
                auto const raw_bucket = elapsed / latency_bucket_ns;
                auto const bucket = raw_bucket < buckets.size() ? raw_bucket : buckets.size() - 1;
                buckets[bucket].fetch_add(1, std::memory_order_relaxed);
            }
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

    [[nodiscard]] std::uint64_t transaction_p99_upper_ns() const noexcept
    {
        std::array<std::uint64_t, latency_bucket_count> buckets{};
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < buckets.size(); ++i) {
            buckets[i] = transaction_latency_[i].load(std::memory_order_relaxed);
            total += buckets[i];
        }
        if (!total) return 0;
        auto const rank = total - total / 100;
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i + 1 < buckets.size(); ++i) {
            cumulative += buckets[i];
            if (cumulative >= rank) return (i + 1) * latency_bucket_ns;
        }
        return phases_[static_cast<std::size_t>(PlaybackPreparationPhase::transaction)].max_ns.load(std::memory_order_relaxed);
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
