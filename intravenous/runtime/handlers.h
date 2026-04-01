#pragma once

#include "../compat.h"

#include <cstdlib>
#include <exception>
#include <csignal>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace iv {
    using ShutdownHandler = void (*)();

    inline ShutdownHandler g_shutdown_handler = nullptr;

    [[noreturn]] inline void terminate_with_stacktrace()
    {
        auto& out = diagnostic_stream();
        out << "\nstd::terminate called\n";

        if (auto ep = std::current_exception()) {
            try {
                std::rethrow_exception(ep);
            } catch (std::exception const& e) {
                print_stacktrace(out);
                out << "uncaught exception: " << e.what() << '\n';
            } catch (...) {
                print_stacktrace(out);
                out << "uncaught non-std exception\n";
            }
        } else {
            print_stacktrace(out);
            out << "no active exception\n";
        }
        out.flush();

        std::abort();
    }

#if defined(_WIN32)
    inline LONG WINAPI unhandled_exception_stacktrace_filter(EXCEPTION_POINTERS* info)
    {
        auto& out = diagnostic_stream();
        out << '\n';
        print_stacktrace(out);
        out << "unhandled exception 0x" << std::hex
            << (info ? info->ExceptionRecord->ExceptionCode : 0)
            << std::dec << '\n';
        if (auto ep = std::current_exception()) {
            try {
                std::rethrow_exception(ep);
            } catch (std::exception const& e) {
                out << "uncaught exception: " << e.what() << '\n';
            } catch (...) {
                out << "uncaught non-std exception\n";
            }
        }
        out.flush();
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

    inline void dispatch_shutdown_handler()
    {
        if (g_shutdown_handler) {
            g_shutdown_handler();
        }
    }

    inline void handle_sigint(int)
    {
        dispatch_shutdown_handler();
    }

#if defined(_WIN32)
    inline BOOL WINAPI handle_console_ctrl(DWORD ctrl_type)
    {
        if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
            dispatch_shutdown_handler();
            return TRUE;
        }
        return FALSE;
    }
#endif

    inline void install_shutdown_handlers(ShutdownHandler handler)
    {
        g_shutdown_handler = handler;
        std::signal(SIGINT, handle_sigint);
#if defined(_WIN32)
        SetConsoleCtrlHandler(handle_console_ctrl, TRUE);
#endif
    }
}
