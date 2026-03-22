#pragma once

#include "module/loader.h"
#include "runtime/system.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
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

    inline std::filesystem::path runtime_modules_root()
    {
        return repo_root() / "build" / "test_runtime_modules";
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

    inline iv::NodeProcessor make_processor(iv::ModuleLoader& loader, iv::System& system, std::filesystem::path const& module_path)
    {
        auto graph = loader.load_root(module_path, system);
        system.activate_root(graph.sink_count != 0);
        return iv::NodeProcessor(
            system.wrap_root(std::move(graph.root), graph.sink_count != 0),
            std::move(graph.module_refs)
        );
    }

    inline void run_processor_ticks(iv::NodeProcessor& processor, size_t ticks = 16)
    {
        for (size_t i = 0; i < ticks; ++i) {
            processor.tick({}, i);
        }
    }
}
