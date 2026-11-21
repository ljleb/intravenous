#pragma once

#include <intravenous/execution/project_playback.h>
#include <intravenous/sample.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <span>

namespace iv {

// The renderer tries the guard once per device buffer during a capture. It
// neither waits nor allocates. The control side retries a read after the last
// in-flight buffer finishes.
class PlaybackOutputProfile {
public:
    using Clock = std::chrono::steady_clock;

    struct PreviousSlice {
        std::uint64_t capture = 0;
        PlaybackRenderSlice slice{};
        std::array<float, 8> tail{};
        std::size_t samples = 0;
    };

private:
    std::mutex control_{};
    std::atomic<bool> recording_{false};
    std::atomic_flag busy_{};
    std::string id_{};
    std::uint64_t capture_ = 0;
    Clock::time_point start_{}, end_{};
    std::array<PlaybackOutputCutover, 256> cutovers_{};
    std::size_t count_ = 0, buffers_ = 0, over_budget_ = 0, omitted_ = 0;
    double max_render_ms_ = 0, max_budget_fraction_ = 0;

    struct Unlock {
        std::atomic_flag& flag;
        ~Unlock() { flag.clear(std::memory_order_release); }
    };

    static double milliseconds(Clock::duration time) noexcept {
        return std::chrono::duration<double, std::milli>(time).count();
    }

public:
    PlaybackOutputProfileResult request(PlaybackOutputProfileRequest const& request) {
        std::scoped_lock lock(control_);
        if (request.duration_ms) {
            if (busy_.test_and_set(std::memory_order_acquire)) return {.status = "busy"};
            Unlock unlock{busy_};
            auto const now = Clock::now();
            if (recording_.load(std::memory_order_relaxed) && now < end_) return {.status = "busy"};
            id_ = request.id;
            ++capture_;
            start_ = now;
            end_ = now + std::chrono::milliseconds(*request.duration_ms);
            count_ = buffers_ = over_budget_ = omitted_ = 0;
            max_render_ms_ = max_budget_fraction_ = 0;
            recording_.store(true, std::memory_order_release);
            return {.status = "recording"};
        }
        if (id_ != request.id) return {.status = "unavailable"};
        recording_.store(false, std::memory_order_release);
        if (busy_.test_and_set(std::memory_order_acquire)) return {.status = "pending"};
        Unlock unlock{busy_};
        return {.status = "ready", .buffers = buffers_, .over_budget_buffers = over_budget_,
            .omitted_cutovers = omitted_, .max_render_ms = max_render_ms_,
            .max_budget_fraction = max_budget_fraction_,
            .cutovers = std::vector<PlaybackOutputCutover>(cutovers_.begin(), cutovers_.begin() + count_)};
    }

    class Buffer {
        PlaybackOutputProfile* profile_ = nullptr;
        std::size_t first_cutover_ = 0;
        std::size_t previous_omitted_ = 0;
        Clock::time_point started_{};
        bool submitted_ = false;

    public:
        explicit Buffer(PlaybackOutputProfile& profile) noexcept {
            if (!profile.recording_.load(std::memory_order_relaxed)
                || profile.busy_.test_and_set(std::memory_order_acquire)) return;
            started_ = Clock::now();
            if (!profile.recording_.load(std::memory_order_acquire) || started_ >= profile.end_) {
                profile.recording_.store(false, std::memory_order_release);
                profile.busy_.clear(std::memory_order_release);
                return;
            }
            profile_ = &profile;
            first_cutover_ = profile.count_;
            previous_omitted_ = profile.omitted_;
        }
        ~Buffer() {
            if (!profile_) return;
            if (!submitted_) {
                profile_->count_ = first_cutover_;
                profile_->omitted_ = previous_omitted_;
            }
            profile_->busy_.clear(std::memory_order_release);
        }
        Buffer(Buffer const&) = delete;
        Buffer& operator=(Buffer const&) = delete;
        explicit operator bool() const noexcept { return profile_ != nullptr; }

        void observe(PreviousSlice& previous, PlaybackRenderSlice const& slice,
            std::span<Sample const> output) noexcept {
            if (previous.capture == profile_->capture_ && previous.samples
                && (previous.slice.execution_generation != slice.execution_generation
                    || previous.slice.code_publication != slice.code_publication
                    || (previous.slice.rendered && slice.rendered
                        && previous.slice.transition_tick != slice.transition_tick))) {
                if (profile_->count_ == profile_->cutovers_.size()) {
                    ++profile_->omitted_;
                } else {
                    auto& cutover = profile_->cutovers_[profile_->count_++];
                    cutover.previous = previous.slice;
                    cutover.current = slice;
                    cutover.before = previous.tail;
                    cutover.before_samples = previous.samples;
                    cutover.after_samples = std::min(cutover.after.size(), output.size());
                    for (std::size_t i = 0; i < cutover.after_samples; ++i) cutover.after[i] = output[i].value;
                    cutover.elapsed_ms = milliseconds(Clock::now() - profile_->start_);
                }
            }
            previous.capture = profile_->capture_;
            previous.slice = slice;
            previous.samples = std::min(previous.tail.size(), output.size());
            auto const tail = output.last(previous.samples);
            for (std::size_t i = 0; i < tail.size(); ++i) previous.tail[i] = tail[i].value;
        }

        void submitted(Clock::time_point rendered_at, std::size_t frames, double sample_rate) noexcept {
            submitted_ = true;
            if (!frames) return;
            auto const render_ms = milliseconds(rendered_at - started_);
            auto const budget_ms = 1000.0 * static_cast<double>(frames) / sample_rate;
            ++profile_->buffers_;
            profile_->over_budget_ += budget_ms > 0 && render_ms > budget_ms;
            profile_->max_render_ms_ = std::max(profile_->max_render_ms_, render_ms);
            if (budget_ms > 0)
                profile_->max_budget_fraction_ = std::max(profile_->max_budget_fraction_, render_ms / budget_ms);
            for (auto i = first_cutover_; i < profile_->count_; ++i) {
                profile_->cutovers_[i].render_ms = render_ms;
                profile_->cutovers_[i].budget_ms = budget_ms;
            }
        }
    };
};

} // namespace iv
