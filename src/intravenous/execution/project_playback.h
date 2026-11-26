#pragma once

#include <intravenous/compat.h>
#include <intravenous/global_index.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace iv {

struct ProjectPlaybackSnapshot {
    GlobalIndex background_playhead{}; // Project position used to select background content.
    GlobalIndex start_position{};
    GlobalIndex foreground_playhead{}; // Starting index of the next Tick block.
    bool running = false;
    bool foreground_running = false;
    std::uint64_t applied_command = 0;
    std::uint64_t segment = 0;
    double sample_rate = 48000;
};

struct TickPlaybackSelection {
    std::optional<GlobalIndex> held_position{};
    bool audition = false;
};

enum class ProjectPlaybackOperation { read, set_running, toggle, toggle_foreground, seek, drag };
enum class ProjectPlaybackDragPhase { begin, move, end };
struct ProjectPlaybackDrag {
    ProjectPlaybackDragPhase phase = ProjectPlaybackDragPhase::begin;
    std::uint64_t id = 0;
};

struct ProjectPlaybackDiagnostics {
    std::uint64_t background_underrun_blocks = 0;
    std::optional<std::uint64_t> configured_generation{};
    std::optional<std::uint64_t> execution_generation{};
};

struct PlaybackOutputProfileRequest {
    std::string id{};
    std::optional<std::uint32_t> duration_ms{};
};

struct PlaybackRenderSlice {
    std::optional<std::uint64_t> execution_generation{};
    GlobalIndex foreground_sample{};
    std::uint64_t segment = 0;
    bool rendered = false;
    std::optional<std::uint64_t> code_publication{};
    bool transition_tick = false;
};

struct PlaybackOutputCutover {
    PlaybackRenderSlice previous{}, current{};
    std::array<float, 8> before{}, after{};
    std::size_t before_samples = 0, after_samples = 0;
    double elapsed_ms = 0, render_ms = 0, budget_ms = 0;
};

struct PlaybackOutputProfileResult {
    std::string status{};
    std::size_t buffers = 0, over_budget_buffers = 0, omitted_cutovers = 0;
    double max_render_ms = 0, max_budget_fraction = 0;
    std::vector<PlaybackOutputCutover> cutovers{};
};

struct ProjectPlaybackRequest {
    ProjectPlaybackOperation operation = ProjectPlaybackOperation::read;
    GlobalIndex position{};
    bool running = true;
    ProjectPlaybackDrag drag{};
    bool include_diagnostics = false;
    std::optional<PlaybackOutputProfileRequest> output_profile{};
    std::optional<std::uint64_t> accepted_command{};
    std::optional<ProjectPlaybackSnapshot> snapshot{};
    std::optional<ProjectPlaybackDiagnostics> diagnostics{};
    std::optional<PlaybackOutputProfileResult> output_profile_result{};
};

// The control side orders commands; the audio side alone applies them. The
// queue has fixed capacity and the published fields are atomic so a control
// reader can obtain a coherent snapshot without making audio wait.
class ProjectPlayback {
public:
    struct Block {
        ProjectPlaybackSnapshot state;
        bool audition = false;

        [[nodiscard]] TickPlaybackSelection selection() const noexcept
        {
            return {state.running ? std::nullopt : std::optional{state.background_playhead}, audition};
        }
    };

    struct SealedForegroundPrefix {
        std::uint64_t execution_generation;
        Block first;
        GlobalIndexRange execution;
    };

private:
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
    // Counts rendered samples even when a command changes project position.
    GlobalIndex rendered_through_{};
    std::size_t sealed_foreground_frames_ = 0;
    enum class ForegroundPrefixPhase : std::uint8_t {
        idle, reserved, executing, ready, consumed, discard_requested, discarded,
    };
    std::atomic<ForegroundPrefixPhase> foreground_prefix_phase_{ForegroundPrefixPhase::idle};
    SealedForegroundPrefix foreground_prefix_{};
    bool provisional_foreground_prefix_ = false;
    std::optional<GlobalIndex> pending_position_{};
    std::optional<GlobalIndex> pending_foreground_position_{};
    bool pending_audition_ = false;
    // Present while the audio side is processing a drag, even before its first position applies.
    std::optional<bool> drag_resume_running_{};
    std::atomic<std::uint64_t> publication_{0};
    std::atomic<std::uint64_t> background_playhead_{0}, start_position_{0}, foreground_playhead_{0};
    std::atomic<std::uint64_t> applied_{0}, segment_{0};
    std::atomic<bool> running_{state_.running};
    std::atomic<bool> foreground_running_{state_.foreground_running};
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
        if (!pending_.foreground_running) pending_foreground_position_ = position;
        pending_.start_position = position;
        pending_audition_ = !pending_.running && pending_.foreground_running;
        ++pending_.segment;
    }

    void publish() noexcept
    {
        publication_.fetch_add(1, std::memory_order_acq_rel);
        background_playhead_.store(state_.background_playhead.bits(), std::memory_order_relaxed);
        start_position_.store(state_.start_position.bits(), std::memory_order_relaxed);
        foreground_playhead_.store(state_.foreground_playhead.bits(), std::memory_order_relaxed);
        running_.store(state_.running, std::memory_order_relaxed);
        foreground_running_.store(state_.foreground_running, std::memory_order_relaxed);
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
    [[nodiscard]] std::uint64_t toggle_foreground()
    {
        return submit({.operation = ProjectPlaybackOperation::toggle_foreground});
    }
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

    // Audio side only. Reserve after the worker has the inputs and storage
    // for this prefix. A seal commits the current playback selection so the
    // worker can advance its state exactly once. Later commands wait for it.
    [[nodiscard]] bool try_seal_foreground_prefix(std::uint64_t execution_generation,
        std::size_t frames, std::size_t maximum_frames, double sample_rate) noexcept
    {
        return try_seal_foreground_prefix(execution_generation, frames, maximum_frames,
            current_block(sample_rate), [] { return true; });
    }

    template<class Prepare>
    [[nodiscard]] bool try_seal_foreground_prefix(std::uint64_t execution_generation,
        std::size_t frames, std::size_t maximum_frames, Block const& first, Prepare prepare,
        bool provisional = false) noexcept
    {
        if (frames == 0 || frames > maximum_frames
            || !first.state.foreground_running
            || (provisional && drag_resume_running_)
            || written_.load(std::memory_order_acquire) != first.state.applied_command
            || foreground_prefix_phase_.load(std::memory_order_acquire) != ForegroundPrefixPhase::idle)
            return false;
        if (!prepare()) return false;
        foreground_prefix_ = {execution_generation, first, {rendered_through_, frames}};
        sealed_foreground_frames_ = frames;
        provisional_foreground_prefix_ = provisional;
        foreground_prefix_phase_.store(ForegroundPrefixPhase::reserved, std::memory_order_release);
        return true;
    }

    // One worker claims the reservation once. It must retain completed
    // output until consumption, including across retries of other publications.
    [[nodiscard]] std::optional<SealedForegroundPrefix> try_begin_foreground_prefix() noexcept
    {
        if (foreground_prefix_phase_.load(std::memory_order_acquire) != ForegroundPrefixPhase::reserved)
            return {};
        foreground_prefix_phase_.store(ForegroundPrefixPhase::executing, std::memory_order_relaxed);
        return foreground_prefix_;
    }

    void publish_foreground_prefix() noexcept
    {
        IV_ASSERT(foreground_prefix_phase_.load(std::memory_order_relaxed) == ForegroundPrefixPhase::executing,
            "foreground prefix publication requires a claimed reservation");
        foreground_prefix_phase_.store(ForegroundPrefixPhase::ready, std::memory_order_release);
    }

    // Transfer storage can be reused after both parts have returned and the
    // foreground has consumed the whole prefix.
    [[nodiscard]] bool release_consumed_foreground_prefix() noexcept
    {
        if (foreground_prefix_phase_.load(std::memory_order_acquire) != ForegroundPrefixPhase::consumed)
            return false;
        foreground_prefix_phase_.store(ForegroundPrefixPhase::idle, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t sealed_foreground_frames() const noexcept
    {
        return sealed_foreground_frames_;
    }

    [[nodiscard]] bool has_provisional_foreground_prefix() const noexcept
    {
        return provisional_foreground_prefix_;
    }

    [[nodiscard]] bool provisional_prefix_matches(Block const& block) const noexcept
    {
        auto const& first = foreground_prefix_.first;
        return provisional_foreground_prefix_
            && block.state.foreground_running && first.state.running == block.state.running
            && first.state.foreground_playhead == block.state.foreground_playhead
            && first.selection().held_position == block.selection().held_position
            && first.audition == block.audition && first.state.sample_rate == block.state.sample_rate;
    }

    [[nodiscard]] bool promote_provisional_prefix(Block const& block) noexcept
    {
        if (!provisional_prefix_matches(block) || !ready_foreground_prefix()) return false;
        foreground_prefix_.first = block;
        provisional_foreground_prefix_ = false;
        return true;
    }

    // A prepared startup can be discarded until its first foreground slice.
    // The worker restores fresh storage before releasing this reservation.
    void request_provisional_prefix_discard() noexcept
    {
        IV_ASSERT(provisional_foreground_prefix_, "only a provisional prefix can be discarded");
        auto ready = ForegroundPrefixPhase::ready;
        (void)foreground_prefix_phase_.compare_exchange_strong(
            ready, ForegroundPrefixPhase::discard_requested, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool provisional_prefix_discard_requested() const noexcept
    {
        return foreground_prefix_phase_.load(std::memory_order_acquire) == ForegroundPrefixPhase::discard_requested;
    }

    void publish_provisional_prefix_discard() noexcept
    {
        foreground_prefix_phase_.store(ForegroundPrefixPhase::discarded, std::memory_order_release);
    }

    void release_discarded_foreground_prefix() noexcept
    {
        if (foreground_prefix_phase_.load(std::memory_order_acquire) != ForegroundPrefixPhase::discarded) return;
        sealed_foreground_frames_ = 0;
        provisional_foreground_prefix_ = false;
        foreground_prefix_phase_.store(ForegroundPrefixPhase::idle, std::memory_order_release);
    }

    // Audio side only. The acquire also makes the worker's output visible
    // for all slices of this prefix, including after a later seek is queued.
    [[nodiscard]] SealedForegroundPrefix const* ready_foreground_prefix() const noexcept
    {
        if (sealed_foreground_frames_ == 0
            || foreground_prefix_phase_.load(std::memory_order_acquire) != ForegroundPrefixPhase::ready)
            return nullptr;
        return &foreground_prefix_;
    }

    [[nodiscard]] std::size_t render_frames(std::size_t maximum_frames) const noexcept
    {
        if (sealed_foreground_frames_ != 0 && sealed_foreground_frames_ < maximum_frames)
            return sealed_foreground_frames_;
        return maximum_frames;
    }

    [[nodiscard]] Block current_block(double sample_rate) const noexcept
    {
        if (provisional_foreground_prefix_) {
            auto state = state_;
            state.sample_rate = sample_rate;
            return {state};
        }
        if (sealed_foreground_frames_ != 0 && sealed_foreground_frames_ == foreground_prefix_.execution.extent)
            return foreground_prefix_.first;
        auto state = state_;
        state.sample_rate = sealed_foreground_frames_ == 0 ? sample_rate : foreground_prefix_.first.state.sample_rate;
        // Once begin_block has seen a drag, hold the last applied position
        // while the drag destination is being prepared.
        if (drag_resume_running_ && sealed_foreground_frames_ == 0) state.running = false;
        return {state};
    }

    [[nodiscard]] Block begin_block(double sample_rate) noexcept
    {
        auto consumed = consumed_.load(std::memory_order_relaxed);
        auto const written = written_.load(std::memory_order_acquire);
        if (pending_.applied_command == state_.applied_command) {
            pending_ = state_;
            pending_position_.reset();
            pending_foreground_position_.reset();
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
                    if (pending_.running) pending_.foreground_running = true;
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
                    if (wanted) pending_.foreground_running = true;
                    pending_position_ = pending_.start_position;
                    pending_audition_ = false;
                    ++pending_.segment;
                }
                break;
            }
            case ProjectPlaybackOperation::toggle_foreground:
                pending_.foreground_running = !pending_.foreground_running;
                if (!pending_.foreground_running) {
                    pending_.running = false;
                    if (drag_resume_running_) *drag_resume_running_ = false;
                    pending_position_ = pending_.start_position;
                    pending_foreground_position_ = pending_.start_position;
                    pending_audition_ = false;
                }
                ++pending_.segment;
                break;
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
        state.foreground_playhead = state.running ? state.background_playhead
            : pending_foreground_position_.value_or(state_.foreground_playhead);
        return {state, pending_audition_};
    }

    void end_block(Block const& executed, std::size_t frames) noexcept
    {
        bool const consumes_prefix = sealed_foreground_frames_ != 0;
        if (consumes_prefix) {
            IV_ASSERT(!provisional_foreground_prefix_, "playback consumed a provisional prefix before resume");
            IV_ASSERT(ready_foreground_prefix(), "playback consumed an unpublished foreground prefix");
            auto const expected = current_block(executed.state.sample_rate);
            IV_ASSERT(executed.state.applied_command == expected.state.applied_command
                    && executed.state.segment == expected.state.segment
                    && executed.state.foreground_playhead == expected.state.foreground_playhead
                    && executed.state.background_playhead == expected.state.background_playhead
                    && executed.state.running == expected.state.running
                    && executed.audition == expected.audition
                    && executed.state.sample_rate == foreground_prefix_.first.state.sample_rate
                    && executed.state.foreground_running,
                "playback selected a different block before its sealed foreground prefix drained");
            IV_ASSERT(frames <= sealed_foreground_frames_, "playback crosses the sealed foreground prefix boundary");
            sealed_foreground_frames_ -= frames;
        }
        state_ = executed.state;
        if (state_.foreground_running) {
            state_.foreground_playhead = state_.foreground_playhead.advanced_by(frames);
            rendered_through_ = rendered_through_.advanced_by(frames);
        }
        if (state_.running) state_.background_playhead = state_.foreground_playhead;
        publish();
        if (consumes_prefix && sealed_foreground_frames_ == 0)
            foreground_prefix_phase_.store(ForegroundPrefixPhase::consumed, std::memory_order_release);
    }

    void align_foreground_after_reset() noexcept
    {
        IV_ASSERT(sealed_foreground_frames_ == 0, "graph reset cannot overtake the sealed foreground prefix");
        state_.foreground_playhead = state_.background_playhead;
        ++state_.segment;
        ++pending_.segment;
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
                .foreground_running = foreground_running_.load(std::memory_order_relaxed),
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
