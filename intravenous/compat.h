#pragma once

#include <cstdlib>
#include <iostream>
#include <stacktrace>

#if defined(_MSC_VER)
#  define IV_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#  define IV_FORCEINLINE inline __attribute__((always_inline))
#else
#  define IV_FORCEINLINE inline
#endif

namespace iv {
    [[noreturn]] inline void assertion_failed(
        char const* expression,
        char const* message,
        char const* file,
        int line
    )
    {
        std::cerr << "\nassertion failed: " << expression;
        if (message && *message) {
            std::cerr << " (" << message << ")";
        }
        std::cerr << "\n" << file << ":" << line << '\n';
        std::cerr << std::stacktrace::current() << '\n';
        std::abort();
    }
}

#define IV_ASSERT(expr, message) \
    do { \
        if (!(expr)) { \
            ::iv::assertion_failed(#expr, message, __FILE__, __LINE__); \
        } \
    } while (false)
