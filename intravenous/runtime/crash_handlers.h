#pragma once

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stacktrace>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace iv {
    [[noreturn]] inline void terminate_with_stacktrace()
    {
        std::cerr << "\nstd::terminate called\n";

        if (auto ep = std::current_exception()) {
            try {
                std::rethrow_exception(ep);
            } catch (std::exception const& e) {
                std::cerr << std::stacktrace::current() << '\n';
                std::cerr << "uncaught exception: " << e.what() << '\n';
            } catch (...) {
                std::cerr << std::stacktrace::current() << '\n';
                std::cerr << "uncaught non-std exception\n";
            }
        } else {
            std::cerr << std::stacktrace::current() << '\n';
            std::cerr << "no active exception\n";
        }

        std::abort();
    }

#if defined(_WIN32)
    inline LONG WINAPI unhandled_exception_stacktrace_filter(EXCEPTION_POINTERS* info)
    {
        std::cerr << "\nunhandled exception 0x" << std::hex
                  << (info ? info->ExceptionRecord->ExceptionCode : 0)
                  << std::dec << '\n';
        std::cerr << std::stacktrace::current() << '\n';
        std::abort();
    }
#endif

    inline void install_crash_handlers()
    {
        std::set_terminate(terminate_with_stacktrace);
#if defined(_WIN32)
        SetUnhandledExceptionFilter(unhandled_exception_stacktrace_filter);
#endif
    }
}
