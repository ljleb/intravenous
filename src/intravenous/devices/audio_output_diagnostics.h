#pragma once

#include <intravenous/sample.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace iv {

// Each stage has one writer. Only one block per second is scanned;
// reporting and formatting belong to a service thread.
struct AudioOutputProbe {
    std::uint64_t blocks = 0;
    std::uint64_t generation = 0;
    std::uint64_t tick_calls = 0;
    std::uint64_t binding_calls = 0;
    std::uint64_t matched_bindings = 0;
    std::uint64_t samples = 0;
    std::uint64_t nonfinite = 0;
    double peak = 0;
    double sum = 0;
    double square_sum = 0;

    void measure(std::span<Sample const> values) noexcept
    {
        samples = values.size();
        nonfinite = 0;
        peak = sum = square_sum = 0;
        for (auto sample : values) {
            auto const value = static_cast<double>(sample.value);
            if (!std::isfinite(value)) { ++nonfinite; continue; }
            peak = std::max(peak, std::abs(value));
            sum += value;
            square_sum += value * value;
        }
    }
};

class AudioOutputProbeSlot {
    // Atomic fields make even an interrupted read data-race-free. Sequential
    // consistency plus the sequence check gives one coherent observation.
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> sequence_{0};
    std::array<std::atomic<std::uint64_t>, 10> fields_{};

public:
    void publish(AudioOutputProbe const& probe) noexcept
    {
        auto const sequence = sequence_.load();
        sequence_.store(sequence + 1);
        std::array values{probe.blocks, probe.generation, probe.tick_calls,
            probe.binding_calls, probe.matched_bindings, probe.samples, probe.nonfinite,
            std::bit_cast<std::uint64_t>(probe.peak), std::bit_cast<std::uint64_t>(probe.sum),
            std::bit_cast<std::uint64_t>(probe.square_sum)};
        for (std::size_t i = 0; i < values.size(); ++i) fields_[i].store(values[i]);
        sequence_.store(sequence + 2);
    }

    [[nodiscard]] std::optional<AudioOutputProbe> read() const noexcept
    {
        auto const before = sequence_.load();
        if (!before || before % 2) return std::nullopt;
        std::array<std::uint64_t, 10> values{};
        for (std::size_t i = 0; i < values.size(); ++i) values[i] = fields_[i].load();
        if (sequence_.load() != before) return std::nullopt;
        return AudioOutputProbe{values[0], values[1], values[2], values[3], values[4],
            values[5], values[6], std::bit_cast<double>(values[7]),
            std::bit_cast<double>(values[8]), std::bit_cast<double>(values[9])};
    }
};

struct AudioOutputDiagnostics {
    AudioOutputProbeSlot rendered;
    AudioOutputProbeSlot playback;
};

// Keep one instance in each device/render thread, never share its counters.
class AudioOutputProbeRate {
    std::size_t frames_until_probe_ = 0;
    std::uint64_t blocks_ = 0;
public:
    bool due(std::size_t frames, std::size_t sample_rate) noexcept
    {
        ++blocks_;
        if (frames_until_probe_ > frames) {
            frames_until_probe_ -= frames;
            return false;
        }
        frames_until_probe_ = sample_rate;
        return true;
    }
    [[nodiscard]] std::uint64_t blocks() const noexcept { return blocks_; }
};
} // namespace iv
