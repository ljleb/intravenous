#include <intravenous/project/persistence/project_autosave.h>

#include <intravenous/rpc/socket_rpc_server.h>
#include <intravenous/project/runtime_project_events.h>

namespace iv {
void ProjectAutosave::project_loaded(TimePoint)
{
    std::lock_guard lock(mutex_);
    project_loaded_ = true;
    save_in_flight_ = false;
    first_unsaved_edit_.reset();
    retry_at_ = TimePoint::min();
}

void ProjectAutosave::project_state_changed(TimePoint now)
{
    std::lock_guard lock(mutex_);
    if (!project_loaded_) {
        return;
    }

    if (!first_unsaved_edit_) first_unsaved_edit_ = now;
    last_edit_ = now;
}

void ProjectAutosave::set_enabled(bool enabled, TimePoint now)
{
    std::lock_guard lock(mutex_);
    if (enabled && !enabled_ && first_unsaved_edit_) {
        first_unsaved_edit_ = now - save_interval;
        retry_at_ = TimePoint::min();
    }
    enabled_ = enabled;
}

void ProjectAutosave::handle_socket_rpc_enable_project_autosave(
    EnableProjectAutosaveRequest const &,
    SocketRpcAckResponseBuilder &builder)
{
    set_enabled(true);
    builder.succeed();
}

void ProjectAutosave::handle_socket_rpc_disable_project_autosave(
    DisableProjectAutosaveRequest const &,
    SocketRpcAckResponseBuilder &builder)
{
    set_enabled(false);
    builder.succeed();
}

void ProjectAutosave::handle_socket_rpc_get_autosave_state(
    SessionSnapshotRequest const&,
    SocketRpcAutosaveStateResultBuilder& builder) const
{
    std::scoped_lock lock(mutex_);
    builder.succeed(enabled_, first_unsaved_edit_.has_value() || save_in_flight_);
}

void ProjectAutosave::handle_project_state_changed()
{
    project_state_changed();
}

void ProjectAutosave::handle_project_loaded()
{
    project_loaded();
}

void ProjectAutosave::handle_project_set_autosave_enabled(
    ProjectSetAutosaveEnabledRequest const &request,
    ProjectAckBuilder &builder)
{
    set_enabled(request.enabled);
    builder.succeed();
}

bool ProjectAutosave::enabled() const
{
    std::lock_guard lock(mutex_);
    return enabled_;
}

bool ProjectAutosave::take_due_save(TimePoint now)
{
    std::lock_guard lock(mutex_);
    if (!first_unsaved_edit_ || now < retry_at_
        || (now < *first_unsaved_edit_ + save_interval && now < last_edit_ + idle_delay)) {
        return false;
    }
    return claim_pending_save();
}

bool ProjectAutosave::take_pending_save()
{
    std::lock_guard lock(mutex_);
    return claim_pending_save();
}

bool ProjectAutosave::claim_pending_save()
{
    if (!project_loaded_ || !enabled_ || save_in_flight_ || !first_unsaved_edit_) return false;
    save_in_flight_ = true;
    first_unsaved_edit_.reset();
    return true;
}

void ProjectAutosave::save_succeeded(TimePoint)
{
    std::lock_guard lock(mutex_);
    save_in_flight_ = false;
    retry_at_ = TimePoint::min();
}

void ProjectAutosave::save_failed(TimePoint now)
{
    std::lock_guard lock(mutex_);
    save_in_flight_ = false;
    if (!first_unsaved_edit_) first_unsaved_edit_ = now;
    last_edit_ = now;
    retry_at_ = now + retry_delay;
}
} // namespace iv
