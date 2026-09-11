#include <intravenous/module/loader.h>
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
    iv::ModuleLoader loader(std::filesystem::current_path(), {});
    auto definitions = loader.load_package_definitions(path);
    if (definitions.empty()) {
        throw std::runtime_error("IV package '" + path.string() + "' has no iv modules");
    }
    if (definitions.size() != 1) {
        throw std::runtime_error(
            "IV package '" + path.string()
            + "' provides multiple iv modules; execution benchmark requires one");
    }
    auto& definition = definitions.front();
    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(definition.root),
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
