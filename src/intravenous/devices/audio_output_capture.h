#pragma once

#include <intravenous/sample.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace iv {
// Temporary diagnostic capture. Allocation and snapshots belong to the service
// thread. The device callback only copies into preallocated storage.
class AudioOutputCapture {
public:
    struct Snapshot {
        std::string token{};
        std::size_t sample_rate = 0;
        std::size_t channels = 0;
        std::vector<Sample> samples{};
        // Sample offset, frame count, steady-clock microseconds at handoff.
        std::vector<std::array<std::uint64_t, 3>> blocks{};
    };
private:
    std::atomic<bool> armed_{false};
    std::atomic_flag guard_ = ATOMIC_FLAG_INIT;
    Snapshot data_;
    std::size_t used_ = 0;
    std::size_t blocks_used_ = 0;
    class Guard {
        std::atomic_flag& flag_;
    public:
        explicit Guard(std::atomic_flag& flag) : flag_(flag) {
            while (flag_.test_and_set(std::memory_order_acquire)) {}
        }
        ~Guard() { flag_.clear(std::memory_order_release); }
    };
public:
    void arm(std::string token, std::size_t sample_rate, std::size_t channels) {
        if (sample_rate == 0 || sample_rate > 384000 || channels == 0 || channels > 64)
            throw std::invalid_argument("unsupported diagnostic capture format");
        Snapshot next{.token = std::move(token), .sample_rate = sample_rate, .channels = channels,
            .samples = std::vector<Sample>(sample_rate * channels * 8),
            .blocks = std::vector<std::array<std::uint64_t, 3>>(4096)};
        Guard lock(guard_);
        if (armed_.load(std::memory_order_acquire))
            throw std::runtime_error("an audio diagnostic capture is already recording");
        data_ = std::move(next);
        used_ = blocks_used_ = 0;
        armed_.store(true, std::memory_order_release);
    }
    bool recording() const noexcept { return armed_.load(std::memory_order_acquire); }
    void write(std::span<Sample const> samples, std::size_t rate, std::size_t channels) noexcept {
        if (!armed_.load(std::memory_order_acquire)) return;
        if (guard_.test_and_set(std::memory_order_acquire)) return;
        // arm() is the only other operation allowed while recording. Its
        // pointer swap can delay the first captured block, never a later one.
        if (armed_.load(std::memory_order_relaxed)) {
            if (rate != data_.sample_rate || channels != data_.channels) {
                armed_.store(false, std::memory_order_release);
            } else {
                auto const count = std::min(samples.size(), data_.samples.size() - used_);
                auto const now = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                data_.blocks[blocks_used_++] = {used_, count / channels, static_cast<std::uint64_t>(now)};
                std::copy_n(samples.begin(), count, data_.samples.begin() + used_);
                used_ += count;
                if (used_ == data_.samples.size() || blocks_used_ == data_.blocks.size())
                    armed_.store(false, std::memory_order_release);
            }
        }
        guard_.clear(std::memory_order_release);
    }
    Snapshot snapshot(std::string const& token) {
        if (recording()) throw std::runtime_error("audio diagnostic capture is still recording");
        Guard lock(guard_);
        if (armed_.load(std::memory_order_acquire) || data_.token != token)
            throw std::runtime_error("audio diagnostic capture token is no longer current");
        Snapshot result{.token = data_.token, .sample_rate = data_.sample_rate, .channels = data_.channels};
        result.samples.assign(data_.samples.begin(), data_.samples.begin() + used_);
        result.blocks.assign(data_.blocks.begin(), data_.blocks.begin() + blocks_used_);
        return result;
    }
};
} // namespace iv
