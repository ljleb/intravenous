#pragma once

#include <intravenous/global_index.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace iv {

struct ProjectPlaybackSnapshot {
    GlobalIndex position{};
    bool running = true;
    std::uint64_t applied_command = 0;
    std::uint64_t segment = 0;
    double sample_rate = 48000;
};

struct TickPlaybackSelection {
    std::optional<GlobalIndex> held_position{};
    bool audition = false;
};

enum class ProjectPlaybackOperation { read, set_running, toggle, seek };
struct ProjectPlaybackRequest {
    ProjectPlaybackOperation operation = ProjectPlaybackOperation::read;
    GlobalIndex position{};
    bool running = true;
    std::optional<std::uint64_t> accepted_command{};
    std::optional<ProjectPlaybackSnapshot> snapshot{};
};

// The control side orders commands; the audio side alone applies them. The
// queue has fixed capacity and the published fields are atomic so a control
// reader can obtain a coherent snapshot without making audio wait.
class ProjectPlayback {
    struct Command {
        std::uint64_t sequence = 0;
        GlobalIndex position{};
        bool seek = false;
        bool running = true;
        bool toggle = false;
    };
    static constexpr std::size_t command_capacity = 256;
    std::array<Command, command_capacity> commands_{};
    std::mutex command_mutex_{};
    std::atomic<std::uint64_t> written_{0}, consumed_{0};
    GlobalIndex execution_index_{};
    ProjectPlaybackSnapshot state_{};
    ProjectPlaybackSnapshot pending_{};
    bool pending_seek_ = false;
    std::atomic<std::uint64_t> publication_{0};
    std::atomic<std::uint64_t> position_{0}, applied_{0}, segment_{0};
    std::atomic<bool> running_{true};
    std::atomic<double> sample_rate_{48000};

    std::uint64_t submit(Command command)
    {
        std::lock_guard lock{command_mutex_};
        auto const written = written_.load(std::memory_order_relaxed);
        if (written == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("playback command sequence is exhausted");
        if (written - consumed_.load(std::memory_order_acquire) >= command_capacity)
            throw std::runtime_error("playback command queue is full");
        command.sequence = written + 1;
        commands_[written % command_capacity] = command;
        written_.store(written + 1, std::memory_order_release);
        return command.sequence;
    }

    void publish() noexcept
    {
        publication_.fetch_add(1, std::memory_order_acq_rel);
        position_.store(state_.position.bits(), std::memory_order_relaxed);
        running_.store(state_.running, std::memory_order_relaxed);
        applied_.store(state_.applied_command, std::memory_order_relaxed);
        segment_.store(state_.segment, std::memory_order_relaxed);
        sample_rate_.store(state_.sample_rate, std::memory_order_relaxed);
        publication_.fetch_add(1, std::memory_order_release);
    }

public:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<double>::is_always_lock_free);

    [[nodiscard]] std::uint64_t set_running(bool running) { return submit({.running = running}); }
    [[nodiscard]] std::uint64_t toggle() { return submit({.toggle = true}); }
    [[nodiscard]] std::uint64_t seek(GlobalIndex position) { return submit({.position = position, .seek = true}); }

    struct Block {
        ProjectPlaybackSnapshot state;
        GlobalIndex execution_index;
        bool audition = false;

        [[nodiscard]] TickPlaybackSelection selection() const noexcept
        {
            return {state.running ? std::nullopt : std::optional{state.position}, audition};
        }
    };

    [[nodiscard]] Block current_block(double sample_rate) const noexcept
    {
        auto state = state_;
        state.sample_rate = sample_rate;
        return {state, execution_index_};
    }

    [[nodiscard]] Block begin_block(double sample_rate) noexcept
    {
        auto consumed = consumed_.load(std::memory_order_relaxed);
        auto const written = written_.load(std::memory_order_acquire);
        if (pending_.applied_command == state_.applied_command) pending_ = state_;
        while (consumed != written) {
            auto const& command = commands_[consumed % command_capacity];
            if (command.seek) {
                pending_.position = command.position;
                pending_seek_ = true;
                ++pending_.segment;
            } else if (command.toggle || command.running != pending_.running) {
                pending_.running = command.toggle ? !pending_.running : command.running;
                ++pending_.segment;
            }
            pending_.applied_command = command.sequence;
            ++consumed;
        }
        consumed_.store(consumed, std::memory_order_release);
        auto state = pending_;
        state.sample_rate = sample_rate;
        if (!pending_seek_) state.position = state_.position;
        return {state, state.running ? state.position : execution_index_,
            pending_seek_ && !state.running};
    }

    void end_block(Block const& executed, std::size_t frames) noexcept
    {
        if (executed.state.applied_command == pending_.applied_command) pending_seek_ = false;
        state_ = executed.state;
        execution_index_ = executed.execution_index.advanced_by(frames);
        if (state_.running) state_.position = execution_index_;
        publish();
    }

    [[nodiscard]] ProjectPlaybackSnapshot snapshot() const noexcept
    {
        for (;;) {
            auto const before = publication_.load(std::memory_order_acquire);
            if (before & 1) continue;
            ProjectPlaybackSnapshot result{
                .position = GlobalIndex::from_bits(position_.load(std::memory_order_relaxed)),
                .running = running_.load(std::memory_order_relaxed),
                .applied_command = applied_.load(std::memory_order_relaxed),
                .segment = segment_.load(std::memory_order_relaxed),
                .sample_rate = sample_rate_.load(std::memory_order_relaxed),
            };
            std::atomic_thread_fence(std::memory_order_acquire);
            if (before == publication_.load(std::memory_order_relaxed)) return result;
        }
    }
};

} // namespace iv
