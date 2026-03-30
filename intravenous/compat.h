#pragma once

#include <cstdlib>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <stacktrace>
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
    inline void debug_log(std::string_view message);

    inline bool env_flag_enabled(char const* name)
    {
        char const* value = std::getenv(name);
        if (!value || !*value) {
            return false;
        }

        std::string normalized(value);
        for (char& c : normalized) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return normalized != "0" && normalized != "false" && normalized != "no" && normalized != "off";
    }

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

    inline bool debug_logging_requested()
    {
        if constexpr (!debug_logging_enabled) {
            return false;
        }
        return env_flag_enabled("IV_DEBUG_LOG");
    }

    inline bool sample_trace_enabled()
    {
        if constexpr (!debug_logging_enabled) {
            return false;
        }
        return env_flag_enabled("IV_TRACE_SAMPLES");
    }

    inline std::optional<std::string_view> sample_trace_filter()
    {
        if (!sample_trace_enabled()) {
            return std::nullopt;
        }
        if (char const* value = std::getenv("IV_TRACE_FILTER"); value && *value) {
            return std::string_view(value);
        }
        return std::nullopt;
    }

    inline bool sample_trace_matches(std::string_view text)
    {
        if (!sample_trace_enabled()) {
            return false;
        }
        if (char const* value = std::getenv("IV_TRACE_FILTER"); value && *value) {
            return text.contains(value);
        }
        return true;
    }

    inline bool node_timing_enabled()
    {
#if defined(NDEBUG)
        return false;
#else
        return env_flag_enabled("IV_NODE_TIMING");
#endif
    }

    inline long long node_timing_threshold_us()
    {
        if (char const* value = std::getenv("IV_NODE_TIMING_US"); value && *value) {
            try {
                return std::max(0ll, std::stoll(value));
            } catch (...) {
                return 1000;
            }
        }
        return 1000;
    }

    inline void maybe_log_node_timing(std::string const& message, std::chrono::steady_clock::duration duration)
    {
        if (!node_timing_enabled()) {
            return;
        }

        auto const elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
        if (elapsed_us < node_timing_threshold_us()) {
            return;
        }

        if (char const* value = std::getenv("IV_TRACE_FILTER"); value && *value) {
            std::string_view filter(value);
            if (!std::string_view(message).contains(filter)) {
                return;
            }
        }

        debug_log(message + " elapsed_us=" + std::to_string(elapsed_us));
    }

    inline void debug_log(std::string_view message)
    {
        if (!debug_logging_requested()) {
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
