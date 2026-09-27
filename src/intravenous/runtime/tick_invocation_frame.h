#pragma once

#include <intravenous/graph_jit/tick_invocation_call.h>
#include <intravenous/runtime/persisted_page_store.h>

namespace iv {

// Callback-scoped owner for the narrow generated Tick invocation record.
// Construction performs only the bounded atomic pin operation of a reader slot;
// the slot itself is registered by GraphExecutor on the control path.
class TickInvocationFrame {
    PersistedPageStore::ReaderPin published_pages_{};
    graph_jit::TickInvocationCall call_{};

public:
    explicit TickInvocationFrame(
        PersistedPageStore::ReaderSlot& page_reader) noexcept
        : published_pages_(page_reader.pin())
    {}

    TickInvocationFrame(TickInvocationFrame const&) = delete;
    TickInvocationFrame& operator=(TickInvocationFrame const&) = delete;
    TickInvocationFrame(TickInvocationFrame&&) = delete;
    TickInvocationFrame& operator=(TickInvocationFrame&&) = delete;

    [[nodiscard]] graph_jit::TickInvocationCall const& call() const noexcept
    {
        return call_;
    }

    // Runtime binding construction may inspect the pinned root, but this owner
    // is never passed through the generated ABI.
    [[nodiscard]] PersistedPageStore::Snapshot const& published_pages()
        const noexcept
    {
        return published_pages_.snapshot();
    }
};

} // namespace iv
