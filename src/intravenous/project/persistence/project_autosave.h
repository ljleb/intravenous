#pragma once

#include <chrono>
#include <mutex>
#include <optional>

namespace iv {
class SocketRpcAckResponseBuilder;
class SocketRpcAutosaveStateResultBuilder;
class ProjectAckBuilder;
struct DisableProjectAutosaveRequest;
struct EnableProjectAutosaveRequest;
struct ProjectSetAutosaveEnabledRequest;
struct SessionSnapshotRequest;

    class ProjectAutosave {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

    private:
        static constexpr auto idle_delay = std::chrono::seconds(3);
        static constexpr auto save_interval = std::chrono::seconds(10);
        static constexpr auto retry_delay = std::chrono::seconds(5);

        mutable std::mutex mutex_;
        bool project_loaded_ = false;
        bool enabled_ = true;
        bool save_in_flight_ = false;
        std::optional<TimePoint> first_unsaved_edit_ {};
        TimePoint last_edit_ {};
        TimePoint retry_at_ = TimePoint::min();

        // Called with mutex_ held. Edits after a claim start a new interval.
        [[nodiscard]] bool claim_pending_save();

    public:
        void project_loaded(TimePoint now = Clock::now());
        void project_state_changed(TimePoint now = Clock::now());
        void set_enabled(bool enabled, TimePoint now = Clock::now());
        void handle_socket_rpc_enable_project_autosave(
            EnableProjectAutosaveRequest const &request,
            SocketRpcAckResponseBuilder &builder);
        void handle_socket_rpc_disable_project_autosave(
            DisableProjectAutosaveRequest const &request,
            SocketRpcAckResponseBuilder &builder);
        void handle_socket_rpc_get_autosave_state(
            SessionSnapshotRequest const& request,
            SocketRpcAutosaveStateResultBuilder& builder) const;
        void handle_project_state_changed();
        void handle_project_loaded();
        void handle_project_set_autosave_enabled(
            ProjectSetAutosaveEnabledRequest const &request,
            ProjectAckBuilder &builder);
        [[nodiscard]] bool enabled() const;
        [[nodiscard]] bool take_due_save(TimePoint now = Clock::now());
        // Claim an outstanding save without waiting for the debounce timer.
        // Used after the autosave worker has stopped during server teardown.
        [[nodiscard]] bool take_pending_save();
        void save_succeeded(TimePoint now = Clock::now());
        void save_failed(TimePoint now = Clock::now());
    };
} // namespace iv
