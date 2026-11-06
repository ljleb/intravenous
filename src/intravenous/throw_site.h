#pragma once

#include <exception>
#include <optional>
#include <stacktrace>

namespace iv {
// The stack captured where `failure` was thrown, before any frame unwound.
// Available when the throw went through the process's `__cxa_throw` hook
// (Itanium C++ ABI on Linux), the exception derives from std::exception, and it
// is still among the most recent throws of the calling thread. A catch handler
// that asks for `std::stacktrace::current()` instead sees only the frames that
// survived unwinding.
[[nodiscard]] std::optional<std::stacktrace> throw_site_stacktrace(
    std::exception_ptr const& failure) noexcept;

namespace details {
// Called by the `__cxa_throw` hook for every exception thrown on this thread.
void capture_throw_site(void const* exception_object) noexcept;
} // namespace details
} // namespace iv
