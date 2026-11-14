#include <intravenous/throw_site.h>

#include <algorithm>
#include <array>
#include <cstddef>

namespace iv {
namespace {
struct CapturedThrow {
    void const* exception_object = nullptr;
    std::stacktrace stacktrace{};
};

// A few recent throws: between a throw and the handler that reports it, other
// exceptions may be thrown and caught (for example during unwinding).
constexpr std::size_t captured_throw_count = 8;
thread_local std::array<CapturedThrow, captured_throw_count> captured_throws{};
thread_local std::size_t next_captured_throw = 0;
thread_local std::size_t captured_throw_entries = 0;
// Capturing allocates; an exception thrown while capturing must not recurse.
thread_local bool capturing_throw = false;
} // namespace

namespace details {
void capture_throw_site(void const* exception_object) noexcept
{
    if (capturing_throw) return;
    capturing_throw = true;
    try {
        auto& slot = captured_throws[next_captured_throw];
        slot.exception_object = nullptr;
        // Skips this function and the hook itself.
        slot.stacktrace = std::stacktrace::current(2);
        slot.exception_object = exception_object;
        next_captured_throw = (next_captured_throw + 1) % captured_throw_count;
        captured_throw_entries = std::min(
            captured_throw_entries + 1, captured_throw_count);
    } catch (...) {
    }
    capturing_throw = false;
}
} // namespace details

std::optional<std::stacktrace> throw_site_stacktrace(
    std::exception_ptr const& failure) noexcept
{
    if (!failure) return std::nullopt;
    void const* exception_object = nullptr;
    try {
        // Rethrowing reuses the original exception object and does not pass
        // through `__cxa_throw`, so no capture is replaced.
        std::rethrow_exception(failure);
    } catch (std::exception const& error) {
        exception_object = dynamic_cast<void const*>(&error);
    } catch (...) {
        return std::nullopt;
    }
    try {
        // Exception storage can be reused after an earlier exception is
        // destroyed. Search from the newest capture so a stale entry with the
        // same address cannot supply the wrong throw-site stack.
        for (std::size_t offset = 0; offset < captured_throw_entries; ++offset) {
            auto const index =
                (next_captured_throw + captured_throw_count - 1 - offset)
                % captured_throw_count;
            auto const& captured = captured_throws[index];
            if (exception_object && captured.exception_object == exception_object) {
                return captured.stacktrace;
            }
        }
    } catch (...) {
    }
    return std::nullopt;
}
} // namespace iv
