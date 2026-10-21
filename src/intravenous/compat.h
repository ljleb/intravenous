#pragma once

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <exception>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>

#if defined(_MSC_VER)
#  define IV_FORCEINLINE __forceinline
#  define IV_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#  define IV_FORCEINLINE inline __attribute__((always_inline))
#  define IV_NOINLINE __attribute__((noinline))
#else
#  define IV_FORCEINLINE inline
#  define IV_NOINLINE
#endif

namespace iv {
    std::ofstream& diagnostic_file();
    std::ostream& diagnostic_stream();
    void flush_diagnostic_stream();
    bool diagnostic_flag_enabled(char const* name);
    void print_stacktrace(std::ostream& out);
    std::string wrap_exception(std::string_view context, std::exception const& cause);
    // Records context for an exception which escaped an authored lifecycle
    // callback, then enters the process-wide terminate handler while that
    // exception remains active. This is deliberately fatal: lifecycle callers
    // cannot roll back a partially committed graph generation.
    [[noreturn]] IV_NOINLINE void terminate_node_lifecycle_exception(
        char const* operation,
        char const* node_type,
        std::size_t node_index) noexcept;
    [[noreturn]] void assertion_failed(
        char const* expression,
        char const* message,
        char const* file,
        int line);
}

#define IV_ASSERT(expr, message) \
    do { \
        if (!(expr)) { \
            ::iv::assertion_failed(#expr, message, __FILE__, __LINE__); \
        } \
    } while (false)
