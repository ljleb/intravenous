#pragma once

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>

// Re-enter under the collector so scratch is also reclaimable after a crash.
inline std::filesystem::path benchmark_scratch(int argc, char** argv)
{
    if (auto const* scratch = std::getenv("IV_BUILD_SCRATCH_DIR"); scratch && *scratch) {
        return scratch;
    }
    std::string tool = IV_BENCHMARK_DEV_CACHE;
    std::string operation = "scratch";
    std::string separator = "--";
    auto executable = std::filesystem::read_symlink("/proc/self/exe").string();
    std::vector<char*> command{tool.data(), operation.data(),
                               separator.data(), executable.data()};
    command.insert(command.end(), argv + 1, argv + argc);
    command.push_back(nullptr);
    ::execv(tool.c_str(), command.data());
    throw std::system_error(errno, std::generic_category(), "start benchmark with build cache scratch");
}
