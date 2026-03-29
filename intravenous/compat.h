#pragma once

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stacktrace>
#include <string_view>

#if defined(_MSC_VER)
#  define IV_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#  define IV_FORCEINLINE inline __attribute__((always_inline))
#else
#  define IV_FORCEINLINE inline
#endif

namespace iv {
    inline std::ostream& diagnostic_stream()
    {
        static std::mutex mutex;
        static std::ofstream file;
        static bool initialized = false;

        std::lock_guard lock(mutex);
        if (!initialized) {
            if (char const* path = std::getenv("IV_LOG_FILE"); path && *path) {
                file.open(path, std::ios::app);
            }
            initialized = true;
        }

        if (file.is_open()) {
            return file;
        }
        return std::cerr;
    }

    inline void print_stacktrace(std::ostream& out)
    {
        out << std::stacktrace::current() << '\n';
        out.flush();
    }

#if defined(NDEBUG)
    inline constexpr bool debug_logging_enabled = false;
#else
    inline constexpr bool debug_logging_enabled = true;
#endif

    inline void debug_log(std::string_view message)
    {
        if constexpr (!debug_logging_enabled) {
            return;
        }

        static std::mutex mutex;
        std::lock_guard lock(mutex);
        auto& out = diagnostic_stream();
        out << message << '\n';
        out.flush();
    }

    [[noreturn]] inline void assertion_failed(
        char const* expression,
        char const* message,
        char const* file,
        int line
    )
    {
        auto& out = diagnostic_stream();
        out << '\n';
        print_stacktrace(out);
        out << "assertion failed: " << expression;
        if (message && *message) {
            out << " (" << message << ")";
        }
        out << "\n" << file << ":" << line << '\n';
        out.flush();
        std::abort();
    }
}

#define IV_ASSERT(expr, message) \
    do { \
        if (!(expr)) { \
            ::iv::assertion_failed(#expr, message, __FILE__, __LINE__); \
        } \
    } while (false)
