#pragma once

#include "module/loader.h"
#include "runtime/crash_handlers.h"
#include "runtime/system.h"

#include <chrono>
#include <cstdlib>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace iv::test {
    inline std::filesystem::path repo_root()
    {
        return std::filesystem::path(__FILE__).parent_path().parent_path();
    }

    inline std::filesystem::path test_modules_root()
    {
        return repo_root() / "tests" / "test_modules";
    }

    inline std::filesystem::path duplicate_modules_root()
    {
        return repo_root() / "tests" / "test_modules_duplicate";
    }

    inline std::filesystem::path runtime_modules_root()
    {
        return repo_root() / "build" / "test_runtime_modules";
    }

    inline std::filesystem::path runtime_module_cache_root()
    {
        return repo_root() / "build" / "iv_runtime_modules";
    }

    inline std::string sanitize_module_id(std::string_view id)
    {
        std::string sanitized(id);
        if (sanitized.empty()) {
            sanitized = "module";
        }

        for (char& c : sanitized) {
            bool good =
                (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9');
            if (!good) {
                c = '_';
            }
        }

        return sanitized;
    }

    inline char const* active_build_config()
    {
#if defined(NDEBUG)
        return "Release";
#else
        return "Debug";
#endif
    }

    inline std::string stable_path_hash(std::filesystem::path const& path)
    {
        auto normalized = std::filesystem::weakly_canonical(path).generic_string();
        uint64_t hash = 1469598103934665603ull;
        for (unsigned char c : normalized) {
            hash ^= c;
            hash *= 1099511628211ull;
        }

        std::ostringstream out;
        out << std::hex << hash;
        return out.str();
    }

    inline std::filesystem::path runtime_module_workspace(std::string_view id, std::filesystem::path const& module_dir)
    {
        return runtime_module_cache_root() / (sanitize_module_id(id) + "_" + stable_path_hash(module_dir)) / active_build_config();
    }

    inline void require(bool condition, char const* message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            std::exit(1);
        }
    }

    inline void require_contains(std::string const& text, std::string const& needle, char const* message)
    {
        require(text.contains(needle), message);
    }

    inline std::string read_text(std::filesystem::path const& path)
    {
        std::ifstream in(path, std::ios::binary);
        require(static_cast<bool>(in), "failed to open input file");
        return std::string(
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()
        );
    }

    inline void write_text(std::filesystem::path const& path, std::string const& text)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(out), "failed to open output file");
        out << text;
    }

    inline std::filesystem::file_time_type write_time(std::filesystem::path const& path)
    {
        std::error_code ec;
        auto stamp = std::filesystem::last_write_time(path, ec);
        require(!ec, "failed to read timestamp");
        return stamp;
    }

    inline void copy_directory(std::filesystem::path const& from, std::filesystem::path const& to)
    {
        std::filesystem::create_directories(to);
        for (auto const& entry : std::filesystem::recursive_directory_iterator(from)) {
            auto relative = std::filesystem::relative(entry.path(), from);
            auto target = to / relative;
            if (entry.is_directory()) {
                std::filesystem::create_directories(target);
            } else if (entry.is_regular_file()) {
                std::filesystem::create_directories(target.parent_path());
                std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::overwrite_existing);
            }
        }
    }

    template<typename Fn>
    inline void expect_failure(Fn&& fn, std::string const& needle, char const* message)
    {
        try {
            fn();
        } catch (std::exception const& e) {
            require_contains(e.what(), needle, message);
            return;
        }

        std::cerr << message << '\n';
        std::exit(1);
    }

    inline iv::ModuleLoader make_loader(std::vector<std::filesystem::path> extra_roots = { test_modules_root() })
    {
        return iv::ModuleLoader(repo_root(), std::move(extra_roots));
    }

    inline iv::NodeProcessor make_processor(iv::ModuleLoader& loader, iv::System& system, std::filesystem::path const& module_path)
    {
        auto graph = loader.load_root(module_path, system);
        system.activate_root(graph.sink_count != 0);
        return system.make_processor(std::move(graph.root), std::move(graph.module_refs));
    }

    inline void run_processor_ticks(iv::NodeProcessor& processor, size_t ticks = 16)
    {
        for (size_t i = 0; i < ticks; ++i) {
            processor.tick_block({}, i, 1);
        }
    }

    inline void run_processor_blocks(
        iv::NodeProcessor& processor,
        std::span<size_t const> block_sizes,
        size_t start_index = 0
    )
    {
        size_t index = start_index;
        for (size_t block_size : block_sizes) {
            processor.tick_block({}, index, block_size);
            index += block_size;
        }
    }

    inline void install_crash_handlers()
    {
        iv::install_crash_handlers();
    }
}
