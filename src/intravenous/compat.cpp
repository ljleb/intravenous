#include <intravenous/compat.h>

#include <fstream>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stacktrace>

namespace iv {
std::ofstream& diagnostic_file()
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

std::ostream& diagnostic_stream()
{
    auto& file = diagnostic_file();
    if (file.is_open()) return file;
    return std::cerr;
}

void flush_diagnostic_stream()
{
    auto& file = diagnostic_file();
    if (file.is_open()) file.flush();
    else std::cerr.flush();
}

bool diagnostic_flag_enabled(char const* name)
{
    if (name == nullptr || *name == '\0') return false;
    if (char const* value = std::getenv(name); value && *value) {
        return std::string_view(value) != "0";
    }
    return false;
}

void print_stacktrace(std::ostream& out)
{
    out << std::stacktrace::current() << '\n';
    out.flush();
}

std::string wrap_exception(std::string_view context, std::exception const& cause)
{
    std::string wrapped(context);
    wrapped += "\ncaused by: ";
    wrapped += cause.what();
    return wrapped;
}

[[noreturn]] void assertion_failed(
    char const* expression,
    char const* message,
    char const* file,
    int line)
{
    auto& out = diagnostic_stream();
    out << '\n';
    print_stacktrace(out);
    out << "assertion failed: " << expression;
    if (message && *message) out << " (" << message << ")";
    out << "\n" << file << ":" << line << '\n';
    out.flush();
    std::abort();
}
} // namespace iv
