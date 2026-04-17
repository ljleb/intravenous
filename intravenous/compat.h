#pragma once

#include <fstream>
#include <exception>
#include <iostream>
#include <mutex>
#include <stacktrace>
#include <cstdlib>
#include <string>
#include <string_view>

#if defined(_MSC_VER)
#  define IV_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#  define IV_FORCEINLINE inline __attribute__((always_inline))
#else
#  define IV_FORCEINLINE inline
#endif

namespace iv {
    inline std::ofstream& diagnostic_file()
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

        return file;
    }

    inline std::ostream& diagnostic_stream()
    {
        auto& file = diagnostic_file();
        if (file.is_open()) {
            return file;
        }
        return std::cerr;
    }

    inline void flush_diagnostic_stream()
    {
        auto& file = diagnostic_file();
        if (file.is_open()) {
            file.flush();
        } else {
            std::cerr.flush();
        }
    }

    inline bool diagnostic_flag_enabled(char const* name)
    {
        if (name == nullptr || *name == '\0') {
            return false;
        }
        if (char const* value = std::getenv(name); value && *value) {
            return std::string_view(value) != "0";
        }
        return false;
    }

    inline void print_stacktrace(std::ostream& out)
    {
        out << std::stacktrace::current() << '\n';
        out.flush();
    }

    inline std::string wrap_exception(std::string_view context, std::exception const& cause)
    {
        std::string wrapped(context);
        wrapped += "\ncaused by: ";
        wrapped += cause.what();
        return wrapped;
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
