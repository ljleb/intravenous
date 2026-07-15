#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace iv {
    class ProjectAutosave {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

    private:
        static constexpr auto debounce_delay = std::chrono::seconds(1);
        static constexpr auto retry_delay = std::chrono::seconds(5);

        mutable std::mutex mutex_;
        bool project_loaded_ = false;
        bool enabled_ = true;
        bool save_in_flight_ = false;
        std::uint64_t revision_ = 0;
        std::uint64_t saving_revision_ = 0;
        std::optional<TimePoint> due_at_ {};

    public:
        void project_loaded(TimePoint now = Clock::now());
        void project_state_changed(TimePoint now = Clock::now());
        void set_enabled(bool enabled, TimePoint now = Clock::now());
        [[nodiscard]] bool enabled() const;
        [[nodiscard]] bool take_due_save(TimePoint now = Clock::now());
        // Claim an outstanding save without waiting for the debounce timer.
        // Used after the autosave worker has stopped during server teardown.
        [[nodiscard]] bool take_pending_save();
        void save_succeeded(TimePoint now = Clock::now());
        void save_failed(TimePoint now = Clock::now());
    };
} // namespace iv
