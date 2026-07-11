#include <intravenous/runtime/project_autosave.h>

namespace iv {
void ProjectAutosave::project_loaded(TimePoint)
{
    std::lock_guard lock(mutex_);
    project_loaded_ = true;
    save_in_flight_ = false;
    revision_ = 0;
    saving_revision_ = 0;
    due_at_.reset();
}

void ProjectAutosave::project_state_changed(TimePoint now)
{
    std::lock_guard lock(mutex_);
    if (!project_loaded_) {
        return;
    }

    ++revision_;
    if (enabled_ && !save_in_flight_) {
        due_at_ = now + debounce_delay;
    }
}

void ProjectAutosave::set_enabled(bool enabled, TimePoint now)
{
    std::lock_guard lock(mutex_);
    enabled_ = enabled;
    if (!enabled_) {
        due_at_.reset();
    } else if (project_loaded_ && revision_ != saving_revision_ && !save_in_flight_) {
        due_at_ = now;
    }
}

bool ProjectAutosave::enabled() const
{
    std::lock_guard lock(mutex_);
    return enabled_;
}

bool ProjectAutosave::take_due_save(TimePoint now)
{
    std::lock_guard lock(mutex_);
    if (!project_loaded_ || !enabled_ || save_in_flight_ || !due_at_.has_value() || now < *due_at_) {
        return false;
    }

    due_at_.reset();
    save_in_flight_ = true;
    saving_revision_ = revision_;
    return true;
}

void ProjectAutosave::save_succeeded(TimePoint now)
{
    std::lock_guard lock(mutex_);
    save_in_flight_ = false;
    if (revision_ == saving_revision_) {
        return;
    }
    if (enabled_) {
        due_at_ = now + debounce_delay;
    }
}

void ProjectAutosave::save_failed(TimePoint now)
{
    std::lock_guard lock(mutex_);
    save_in_flight_ = false;
    if (enabled_) {
        due_at_ = now + retry_delay;
    }
}
} // namespace iv
