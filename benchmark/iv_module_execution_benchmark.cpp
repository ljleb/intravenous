#include <intravenous/module/abi.h>
#include <intravenous/graph/authored_graph_view.hpp>
#include <intravenous/graph/builder/lowering.hpp>
#include <intravenous/graph/compiler.h>
#include <intravenous/node/block_executor.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::vector<std::filesystem::path> modules;
    size_t warmup_blocks = 4096;
    size_t measured_blocks = 131072;
    size_t blocks_per_sample = 256;
    size_t block_size = 64;
    size_t sample_rate = 48000;
};

class DynamicLibrary {
#if defined(_WIN32)
    HMODULE handle_ = nullptr;
#else
    void* handle_ = nullptr;
#endif

public:
    explicit DynamicLibrary(std::filesystem::path const& path)
    {
#if defined(_WIN32)
        handle_ = LoadLibraryW(path.c_str());
        if (!handle_) {
            throw std::runtime_error("LoadLibraryW failed for '" + path.string() + "'");
        }
#else
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_) {
            throw std::runtime_error(
                "dlopen failed for '" + path.string() + "': " + dlerror());
        }
#endif
    }

    ~DynamicLibrary()
    {
#if defined(_WIN32)
        if (handle_) FreeLibrary(handle_);
#else
        if (handle_) dlclose(handle_);
#endif
    }

    DynamicLibrary(DynamicLibrary const&) = delete;
    DynamicLibrary& operator=(DynamicLibrary const&) = delete;

    void* symbol(char const* name) const
    {
#if defined(_WIN32)
        return reinterpret_cast<void*>(GetProcAddress(handle_, name));
#else
        return dlsym(handle_, name);
#endif
    }
};

size_t parse_size(std::string_view value, char const* option)
{
    size_t result = 0;
    auto const [end, error] =
        std::from_chars(value.begin(), value.end(), result);
    if (error != std::errc{} || end != value.end() || result == 0) {
        throw std::runtime_error(
            "expected a positive integer after " + std::string(option));
    }
    return result;
}

Options parse_options(int argc, char** argv)
{
    Options options;
    auto require_value = [&](int& index, char const* option) {
        if (++index == argc) {
            throw std::runtime_error("missing value after " + std::string(option));
        }
        return std::string_view(argv[index]);
    };

    for (int index = 1; index < argc; ++index) {
        std::string_view const arg = argv[index];
        if (arg == "--module") {
            options.modules.emplace_back(require_value(index, "--module"));
        } else if (arg == "--warmup-blocks") {
            options.warmup_blocks = parse_size(
                require_value(index, "--warmup-blocks"), "--warmup-blocks");
        } else if (arg == "--blocks") {
            options.measured_blocks = parse_size(
                require_value(index, "--blocks"), "--blocks");
        } else if (arg == "--blocks-per-sample") {
            options.blocks_per_sample = parse_size(
                require_value(index, "--blocks-per-sample"), "--blocks-per-sample");
        } else if (arg == "--block-size") {
            options.block_size = parse_size(
                require_value(index, "--block-size"), "--block-size");
        } else if (arg == "--sample-rate") {
            options.sample_rate = parse_size(
                require_value(index, "--sample-rate"), "--sample-rate");
        } else if (arg == "--help") {
            std::cout
                << "Usage: iv_module_execution_benchmark --module PATH [--module PATH ...]"
                << " [--warmup-blocks N] [--blocks N] [--blocks-per-sample N]"
                << " [--block-size N] [--sample-rate N]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown option '" + std::string(arg) + "'");
        }
    }
    if (options.modules.empty()) {
        throw std::runtime_error("at least one --module PATH is required");
    }
    if (options.blocks_per_sample > options.measured_blocks) {
        options.blocks_per_sample = options.measured_blocks;
    }
    iv::validate_block_size(options.block_size);
    return options;
}

double percentile(std::vector<double> values, double percentile)
{
    std::sort(values.begin(), values.end());
    auto const index = static_cast<size_t>(
        percentile * static_cast<double>(values.size() - 1));
    return values[index];
}

void benchmark_module(std::filesystem::path const& path, Options const& options)
{
    DynamicLibrary library(path);
    auto const abi_version = reinterpret_cast<iv_module_abi_version_fn>(
        library.symbol("iv_module_abi_version"));
    auto const authored_graph = reinterpret_cast<iv_module_authored_graph_fn>(
        library.symbol("iv_module_authored_graph"));
    if (!abi_version || !authored_graph) {
        throw std::runtime_error("module '" + path.string() + "' is missing IV exports");
    }
    if (abi_version() != iv::IV_MODULE_ABI_VERSION) {
        throw std::runtime_error("module '" + path.string() + "' has an incompatible ABI");
    }

    auto view = authored_graph();
    auto authored = iv::thaw_authored_graph(view);
    auto plan = iv::GraphCompiler::compile(
        iv::GraphLowerer::lower(
            std::move(authored), {.execution_root = true}));
    auto root = std::make_shared<iv::RuntimeGraphRoot>(
        std::move(plan.graph));
    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(*root),
        options.block_size,
        {},
        std::nullopt,
        iv::DEFAULT_EVENT_PORT_BUFFER_BASE_MULTIPLIER,
        options.sample_rate);

    size_t block_index = 0;
    auto tick_blocks = [&](size_t count) {
        for (size_t block = 0; block < count; ++block) {
            executor.tick_block(block_index);
            block_index += options.block_size;
        }
    };

    tick_blocks(options.warmup_blocks);

    std::vector<double> samples_ns_per_block;
    size_t remaining = options.measured_blocks;
    auto const started_at = Clock::now();
    while (remaining != 0) {
        size_t const count = std::min(remaining, options.blocks_per_sample);
        auto const sample_started_at = Clock::now();
        tick_blocks(count);
        auto const elapsed = Clock::now() - sample_started_at;
        samples_ns_per_block.push_back(
            std::chrono::duration<double, std::nano>(elapsed).count()
            / static_cast<double>(count));
        remaining -= count;
    }
    auto const elapsed = Clock::now() - started_at;
    double const mean_ns_per_block =
        std::chrono::duration<double, std::nano>(elapsed).count()
        / static_cast<double>(options.measured_blocks);
    double const blocks_per_second = 1'000'000'000.0 / mean_ns_per_block;
    double const realtime_factor = blocks_per_second
        / (static_cast<double>(options.sample_rate) / options.block_size);

    std::cout << std::fixed << std::setprecision(2)
              << "iv-module-execution-benchmark"
              << " module=" << path
              << " block_size=" << options.block_size
              << " warmup_blocks=" << options.warmup_blocks
              << " measured_blocks=" << options.measured_blocks
              << " mean_ns_per_block=" << mean_ns_per_block
              << " p50_ns_per_block=" << percentile(samples_ns_per_block, 0.50)
              << " p95_ns_per_block=" << percentile(samples_ns_per_block, 0.95)
              << " p99_ns_per_block=" << percentile(samples_ns_per_block, 0.99)
              << " blocks_per_second=" << blocks_per_second
              << " realtime_factor=" << realtime_factor
              << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    try {
        auto const options = parse_options(argc, argv);
        for (auto const& module : options.modules) {
            benchmark_module(module, options);
        }
    } catch (std::exception const& error) {
        std::cerr << "iv-module-execution-benchmark: " << error.what() << '\n';
        return 1;
    }
}
