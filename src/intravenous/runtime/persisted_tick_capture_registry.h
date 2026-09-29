#pragma once

#include <intravenous/runtime/persisted_page_store.h>
#include <intravenous/runtime/tick_capture_store.h>

#include <cstddef>
#include <memory>

namespace iv {

// Executor-lived adapter between generic capture-output handles and canonical
// persisted outputs. The capture transport itself deliberately knows nothing
// about persisted-page or explicit-recorder identity.
class PersistedTickCaptureRegistry {
    class Impl;
    std::unique_ptr<Impl> impl_{};

public:
    explicit PersistedTickCaptureRegistry(TickCaptureStore& captures);
    ~PersistedTickCaptureRegistry();

    PersistedTickCaptureRegistry(PersistedTickCaptureRegistry const&) = delete;
    PersistedTickCaptureRegistry& operator=(
        PersistedTickCaptureRegistry const&) = delete;
    PersistedTickCaptureRegistry(PersistedTickCaptureRegistry&&) = delete;
    PersistedTickCaptureRegistry& operator=(
        PersistedTickCaptureRegistry&&) = delete;

    [[nodiscard]] TickCaptureOutputHandle register_output(
        PersistedOutputId output);
    [[nodiscard]] PersistedOutputId const* persisted_output(
        TickCaptureOutputHandle capture) const;

    [[nodiscard]] TickCaptureStore& capture_store() noexcept;
    [[nodiscard]] TickCaptureStore const& capture_store() const noexcept;
    [[nodiscard]] std::size_t size() const;
};

} // namespace iv
