#include <intravenous/runtime/persisted_tick_capture_registry.h>

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace iv {

class PersistedTickCaptureRegistry::Impl {
public:
    struct Entry {
        TickCaptureOutputHandle capture{};
        PersistedOutputId output{};
    };

    explicit Impl(TickCaptureStore& selected_captures) noexcept
        : captures(&selected_captures)
    {}

    TickCaptureStore* captures = nullptr;
    mutable std::mutex mutex{};
    std::vector<std::unique_ptr<Entry const>> entries{};
};

PersistedTickCaptureRegistry::PersistedTickCaptureRegistry(
    TickCaptureStore& captures)
    : impl_(std::make_unique<Impl>(captures))
{}

PersistedTickCaptureRegistry::~PersistedTickCaptureRegistry() = default;

TickCaptureOutputHandle PersistedTickCaptureRegistry::register_output(
    PersistedOutputId output)
{
    std::scoped_lock lock(impl_->mutex);
    auto const found = std::ranges::find_if(
        impl_->entries,
        [&](auto const& entry) { return entry->output == output; });
    if (found != impl_->entries.end()) return (*found)->capture;

    auto entry = std::make_unique<Impl::Entry const>(Impl::Entry{
        .capture = impl_->captures->register_output(
            persisted_output_kind(output)),
        .output = std::move(output),
    });
    auto const capture = entry->capture;
    impl_->entries.push_back(std::move(entry));
    return capture;
}

PersistedOutputId const* PersistedTickCaptureRegistry::persisted_output(
    TickCaptureOutputHandle capture) const
{
    std::scoped_lock lock(impl_->mutex);
    auto const found = std::ranges::find_if(
        impl_->entries,
        [&](auto const& entry) { return entry->capture == capture; });
    return found == impl_->entries.end()
        ? nullptr
        : &(*found)->output;
}

TickCaptureStore& PersistedTickCaptureRegistry::capture_store() noexcept
{
    return *impl_->captures;
}

TickCaptureStore const& PersistedTickCaptureRegistry::capture_store()
    const noexcept
{
    return *impl_->captures;
}

std::size_t PersistedTickCaptureRegistry::size() const
{
    std::scoped_lock lock(impl_->mutex);
    return impl_->entries.size();
}

} // namespace iv
