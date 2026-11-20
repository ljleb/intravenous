#include "cache.h"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <sys/utsname.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
namespace cache = iv::build_cache;
using Arguments = std::span<char* const>;

std::string_view environment(char const* name)
{
    auto const* value = std::getenv(name);
    return value ? value : "";
}

void set_environment(char const* name, std::string const& value)
{
    if (::setenv(name, value.c_str(), 1) != 0) {
        throw std::system_error(errno, std::generic_category(), std::string("Could not set ") + name);
    }
}

void log(std::string const& message)
{
    std::cerr << "iv-dev-cache: " << message << '\n';
}

// Only development commands need an executable search. Include the resolved
// tool locations in the profile so different toolchains get different trees.
fs::path find_tool(char const* name)
{
    auto search = environment("PATH");
    while (true) {
        auto const delimiter = search.find(':');
        auto const candidate = fs::path(search.substr(0, delimiter)) / name;
        if (::access(candidate.c_str(), X_OK) == 0 && fs::is_regular_file(candidate)) {
            return fs::canonical(candidate);
        }
        if (delimiter == std::string_view::npos) break;
        search.remove_prefix(delimiter + 1);
    }
    throw std::runtime_error(std::string("Could not find ") + name + " on PATH");
}

fs::path server_workspace(fs::path const& root, fs::path const& source, std::string_view build_type)
{
    struct utsname system {};
    if (::uname(&system) != 0) throw std::system_error(errno, std::generic_category(), "Could not read host architecture");
    std::ostringstream profile;
    profile << "Ninja\n" << build_type << '\n' << system.machine << '\n'
            << find_tool("cmake").string() << '\n' << find_tool("ninja").string() << '\n';
    for (auto const* name : {"CC", "CXX", "CMAKE_PREFIX_PATH", "CMAKE_TOOLCHAIN_FILE", "JUCE_DIR", "IV_DEV_NIXPKGS"}) {
        profile << environment(name) << '\n';
    }
    return root / "server" / cache::stable_hash(source.string()) / cache::stable_hash(profile.str());
}

int run_cached_command(fs::path const& root, bool server, Arguments arguments)
{
    auto source = fs::current_path();
    std::string build_type = "Release";
    bool use_scratch = !server;
    while (!arguments.empty()) {
        auto const option = std::string_view(arguments.front());
        arguments = arguments.subspan(1);
        if (option == "--") break;
        if (server && option == "--scratch") {
            use_scratch = true;
        } else if (server && (option == "--source" || option == "--build-type")) {
            if (arguments.empty()) throw std::invalid_argument("Missing value for " + std::string(option));
            if (option == "--source") source = arguments.front();
            else build_type = arguments.front();
            arguments = arguments.subspan(1);
        } else if (!server && option == "--external-build-directory") {
            if (arguments.empty()) throw std::invalid_argument("Missing value for --external-build-directory");
            auto const external = fs::weakly_canonical(fs::absolute(arguments.front()));
            auto const relative = external.lexically_relative(root);
            if (!relative.empty() && *relative.begin() != "..") {
                throw std::invalid_argument("Use the managed server command for a build tree inside the cache");
            }
            arguments = arguments.subspan(1);
        } else {
            throw std::invalid_argument("Unknown option " + std::string(option));
        }
    }
    if (arguments.empty()) throw std::invalid_argument("A command is required after --");
    if (server) {
        source = fs::canonical(source);
        if (!fs::is_regular_file(source / "src/intravenous/CMakeLists.txt")) {
            throw std::invalid_argument("No Intravenous checkout at " + source.string());
        }
    }

    cache::sweep(root, cache::Mode::before_build, log);
    auto run = [&] {
        std::optional<cache::ScratchDirectory> scratch;
        std::optional<cache::Lock> writer, lease;
        std::vector<int> inherited_locks;
        if (use_scratch) {
            scratch.emplace(root);
            set_environment("IV_BUILD_SCRATCH_DIR", scratch->path().string());
            inherited_locks.push_back(scratch->fd());
        }
        fs::path workspace, locks;
        std::string identity;
        if (server) {
            workspace = server_workspace(root, source, build_type);
            locks = root / "locks" / workspace.lexically_relative(root);
            writer.emplace(locks / "write", cache::LockMode::exclusive);
            lease.emplace(locks / "use", cache::LockMode::shared);
            inherited_locks.push_back(writer->fd());
            inherited_locks.push_back(lease->fd());
            identity = source.string() + " [" + build_type + "]";
            cache::mark_used(locks, identity);
            cache::mark_dirty(locks);
            set_environment("IV_SERVER_BUILD_DIR", workspace.string());
            set_environment("IV_SERVER_BUILD_TYPE", build_type);
        }
        std::string descriptors;
        for (auto fd : inherited_locks) {
            if (!descriptors.empty()) descriptors += ' ';
            descriptors += std::to_string(fd);
        }
        set_environment("IV_BUILD_CACHE_LEASE_FDS", descriptors);
        std::vector<char*> command(arguments.begin(), arguments.end());
        command.push_back(nullptr);
        auto const code = cache::run_process(command, inherited_locks);
        if (server) cache::mark_used(locks, identity);
        return code;
    };
    auto finish = [&] {
        try {
            cache::sweep(root, cache::Mode::after_build, log);
        } catch (std::exception const& error) {
            log(error.what());
        }
    };
    try {
        auto const code = run();
        finish();
        return code;
    } catch (...) {
        finish();
        throw;
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        auto const operation = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
        if (operation != "server" && operation != "scratch") {
            throw std::invalid_argument("Expected iv-dev-cache server or scratch");
        }
        return run_cached_command(cache::root(), operation == "server",
            {argv + 2, static_cast<std::size_t>(argc - 2)});
    } catch (std::exception const& error) {
        log(error.what());
        return 1;
    }
}
