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
    GlobalIndex background_playhead{}; // Project position used to select background content.
    GlobalIndex start_position{};
    GlobalIndex foreground_playhead{}; // Starting index of the next Tick block.
    bool running = false;
    std::uint64_t applied_command = 0;
    std::uint64_t segment = 0;
    double sample_rate = 48000;
};

struct TickPlaybackSelection {
    std::optional<GlobalIndex> held_position{};
    bool audition = false;
};

enum class ProjectPlaybackOperation { read, set_running, toggle, seek, drag };
enum class ProjectPlaybackDragPhase { begin, move, end };
struct ProjectPlaybackDrag {
    ProjectPlaybackDragPhase phase = ProjectPlaybackDragPhase::begin;
    std::uint64_t id = 0;
};

struct ProjectPlaybackRequest {
    ProjectPlaybackOperation operation = ProjectPlaybackOperation::read;
    GlobalIndex position{};
    bool running = true;
    ProjectPlaybackDrag drag{};
    std::optional<std::uint64_t> accepted_command{};
    std::optional<ProjectPlaybackSnapshot> snapshot{};
};

// The control side orders commands; the audio side alone applies them. The
// queue has fixed capacity and the published fields are atomic so a control
// reader can obtain a coherent snapshot without making audio wait.
class ProjectPlayback {
    struct Command {
        std::uint64_t sequence = 0;
        ProjectPlaybackOperation operation = ProjectPlaybackOperation::read;
        GlobalIndex position{};
        bool running = true;
        ProjectPlaybackDragPhase drag_phase = ProjectPlaybackDragPhase::begin;
    };
    static constexpr std::size_t command_capacity = 256;
    std::array<Command, command_capacity> commands_{};
    std::mutex command_mutex_{};
    std::optional<std::uint64_t> submitted_drag_{};
    std::atomic<std::uint64_t> written_{0}, consumed_{0};
    ProjectPlaybackSnapshot state_{};
    ProjectPlaybackSnapshot pending_{};
    std::optional<GlobalIndex> pending_position_{};
    bool pending_audition_ = false;
    // Present while the audio side is processing a drag, even before its first position applies.
    std::optional<bool> drag_resume_running_{};
    std::atomic<std::uint64_t> publication_{0};
    std::atomic<std::uint64_t> background_playhead_{0}, start_position_{0}, foreground_playhead_{0};
    std::atomic<std::uint64_t> applied_{0}, segment_{0};
    std::atomic<bool> running_{false};
    std::atomic<double> sample_rate_{48000};

    std::uint64_t enqueue(Command command, bool reserve_drag_end)
    {
        auto const written = written_.load(std::memory_order_relaxed);
        auto const last_sequence = std::numeric_limits<std::uint64_t>::max() - (reserve_drag_end ? 1 : 0);
        if (written >= last_sequence)
            throw std::overflow_error("playback command sequence is exhausted");
        auto const capacity = reserve_drag_end ? command_capacity - 1 : command_capacity;
        if (written - consumed_.load(std::memory_order_acquire) >= capacity)
            throw std::runtime_error("playback command queue is full");
        command.sequence = written + 1;
        commands_[written % command_capacity] = command;
        written_.store(written + 1, std::memory_order_release);
        return command.sequence;
    }

    std::uint64_t submit(Command command)
    {
        std::lock_guard lock{command_mutex_};
        return enqueue(command, submitted_drag_.has_value());
    }

    void select_position(GlobalIndex position) noexcept
    {
        pending_position_ = position;
        pending_.start_position = position;
        pending_audition_ = !pending_.running;
        ++pending_.segment;
    }

    void publish() noexcept
    {
        publication_.fetch_add(1, std::memory_order_acq_rel);
        background_playhead_.store(state_.background_playhead.bits(), std::memory_order_relaxed);
        start_position_.store(state_.start_position.bits(), std::memory_order_relaxed);
        foreground_playhead_.store(state_.foreground_playhead.bits(), std::memory_order_relaxed);
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

    [[nodiscard]] std::uint64_t set_running(bool running)
    {
        return submit({.operation = ProjectPlaybackOperation::set_running, .running = running});
    }
    [[nodiscard]] std::uint64_t toggle() { return submit({.operation = ProjectPlaybackOperation::toggle}); }
    [[nodiscard]] std::uint64_t seek(GlobalIndex position)
    {
        return submit({.operation = ProjectPlaybackOperation::seek, .position = position});
    }
    [[nodiscard]] std::optional<std::uint64_t> drag(ProjectPlaybackDrag drag, GlobalIndex position)
    {
        std::lock_guard lock{command_mutex_};
        if (drag.phase != ProjectPlaybackDragPhase::begin && submitted_drag_ != drag.id) return {};
        auto const ending = drag.phase == ProjectPlaybackDragPhase::end;
        // A drag that has started always has room to release, even when moves fill the queue.
        auto const sequence = enqueue({.operation = ProjectPlaybackOperation::drag, .position = position,
            .drag_phase = drag.phase}, !ending);
        if (ending) submitted_drag_.reset();
        else submitted_drag_ = drag.id;
        return sequence;
    }

    struct Block {
        ProjectPlaybackSnapshot state;
        bool audition = false;

        [[nodiscard]] TickPlaybackSelection selection() const noexcept
        {
            return {state.running ? std::nullopt : std::optional{state.background_playhead}, audition};
        }
    };

    [[nodiscard]] Block current_block(double sample_rate) const noexcept
    {
        auto state = state_;
        state.sample_rate = sample_rate;
        // Once begin_block has seen a drag, hold the last applied position
        // while the drag destination is being prepared.
        if (drag_resume_running_) state.running = false;
        return {state};
    }

    [[nodiscard]] Block begin_block(double sample_rate) noexcept
    {
        auto consumed = consumed_.load(std::memory_order_relaxed);
        auto const written = written_.load(std::memory_order_acquire);
        if (pending_.applied_command == state_.applied_command) {
            pending_ = state_;
            pending_position_.reset();
            pending_audition_ = false;
        }
        while (consumed != written) {
            auto const& command = commands_[consumed % command_capacity];
            switch (command.operation) {
            case ProjectPlaybackOperation::seek:
                select_position(command.position);
                break;
            case ProjectPlaybackOperation::drag:
                if (command.drag_phase == ProjectPlaybackDragPhase::begin) {
                    if (!drag_resume_running_) drag_resume_running_ = pending_.running;
                    pending_.running = false;
                } else if (command.drag_phase == ProjectPlaybackDragPhase::end) {
                    pending_.running = *drag_resume_running_;
                    drag_resume_running_.reset();
                    // Releasing a paused drag at an already selected position preserves its audition.
                    if (!pending_.running && command.position == pending_.start_position) break;
                }
                select_position(command.position);
                break;
            case ProjectPlaybackOperation::set_running:
            case ProjectPlaybackOperation::toggle: {
                auto& running = drag_resume_running_ ? *drag_resume_running_ : pending_.running;
                auto const wanted = command.operation == ProjectPlaybackOperation::toggle ? !running : command.running;
                if (running == wanted) break;
                running = wanted;
                if (!drag_resume_running_) {
                    pending_position_ = pending_.start_position;
                    pending_audition_ = false;
                    ++pending_.segment;
                }
                break;
            }
            case ProjectPlaybackOperation::read:
                break;
            }
            pending_.applied_command = command.sequence;
            ++consumed;
        }
        consumed_.store(consumed, std::memory_order_release);
        auto state = pending_;
        state.sample_rate = sample_rate;
        state.background_playhead = pending_position_.value_or(state_.background_playhead);
        state.foreground_playhead = state.running ? state.background_playhead : state_.foreground_playhead;
        return {state, pending_audition_};
    }

    void end_block(Block const& executed, std::size_t frames) noexcept
    {
        state_ = executed.state;
        state_.foreground_playhead = state_.foreground_playhead.advanced_by(frames);
        if (state_.running) state_.background_playhead = state_.foreground_playhead;
        publish();
    }

    [[nodiscard]] ProjectPlaybackSnapshot snapshot() const noexcept
    {
        for (;;) {
            auto const before = publication_.load(std::memory_order_acquire);
            if (before & 1) continue;
            ProjectPlaybackSnapshot result{
                .background_playhead = GlobalIndex::from_bits(background_playhead_.load(std::memory_order_relaxed)),
                .start_position = GlobalIndex::from_bits(start_position_.load(std::memory_order_relaxed)),
                .foreground_playhead = GlobalIndex::from_bits(foreground_playhead_.load(std::memory_order_relaxed)),
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
