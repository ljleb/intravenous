#include "tests/module_test_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
    struct Options {
        std::filesystem::path module_path;
        size_t block_size = 0;
        size_t warmup_blocks = 8;
        size_t min_measure_blocks = 128;
    };

    [[noreturn]] void usage(std::string_view argv0)
    {
        std::cerr
            << "usage: " << argv0 << " <module_path> [options]\n"
            << "options:\n"
            << "  --block-size N          power-of-two benchmark block size (default: executor max)\n"
            << "  --warmup-blocks N       warmup iterations before measurement (default: 8)\n"
            << "  --min-blocks N          minimum measured iterations (default: 128)\n"
            << "  --min-seconds S         minimum measurement time in seconds (default: 3)\n"
            << "  --max-seconds S         hard measurement cap in seconds (default: 15)\n";
        std::exit(2);
    }

    size_t parse_size(std::string_view text, std::string_view flag)
    {
        try {
            size_t pos = 0;
            std::string owned(text);
            size_t value = std::stoull(owned, &pos);
            if (pos != owned.size()) {
                throw std::invalid_argument("trailing characters");
            }
            return value;
        } catch (std::exception const&) {
            throw std::invalid_argument("invalid value for " + std::string(flag) + ": '" + std::string(text) + "'");
        }
    }

    Options parse_args(int argc, char** argv)
    {
        if (argc < 2) {
            usage(argc > 0 ? argv[0] : "intravenous_render_benchmark");
        }

        Options options;
        options.module_path = argv[1];

        for (int i = 2; i < argc; ++i) {
            std::string_view arg = argv[i];
            auto require_value = [&](std::string_view flag) -> std::string_view {
                if (i + 1 >= argc) {
                    throw std::invalid_argument("missing value for " + std::string(flag));
                }
                return argv[++i];
            };

            if (arg == "--block-size") {
                options.block_size = parse_size(require_value(arg), arg);
            } else if (arg == "--warmup-blocks") {
                options.warmup_blocks = parse_size(require_value(arg), arg);
            } else if (arg == "--min-blocks") {
                options.min_measure_blocks = parse_size(require_value(arg), arg);
            } else if (arg == "--help" || arg == "-h") {
                usage(argv[0]);
            } else {
                throw std::invalid_argument("unknown argument: " + std::string(arg));
            }
        }

        return options;
    }

    struct Stats {
        double min_us = 0.0;
        double max_us = 0.0;
        double mean_us = 0.0;
        double median_us = 0.0;
        double p95_us = 0.0;
        double stddev_us = 0.0;
    };

    Stats compute_stats(std::span<double const> samples_us)
    {
        std::vector<double> sorted(samples_us.begin(), samples_us.end());
        std::sort(sorted.begin(), sorted.end());

        auto percentile = [&](double q) {
            if (sorted.empty()) {
                return 0.0;
            }
            double index = q * static_cast<double>(sorted.size() - 1);
            size_t lo = static_cast<size_t>(std::floor(index));
            size_t hi = static_cast<size_t>(std::ceil(index));
            double mix = index - static_cast<double>(lo);
            return sorted[lo] * (1.0 - mix) + sorted[hi] * mix;
        };

        double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
        double mean = sum / static_cast<double>(sorted.size());
        double sq_sum = 0.0;
        for (double sample : sorted) {
            double delta = sample - mean;
            sq_sum += delta * delta;
        }

        return Stats{
            .min_us = sorted.front(),
            .max_us = sorted.back(),
            .mean_us = mean,
            .median_us = percentile(0.5),
            .p95_us = percentile(0.95),
            .stddev_us = std::sqrt(sq_sum / static_cast<double>(sorted.size())),
        };
    }

    void request_tick_and_drain(iv::test::FakeAudioDevice& audio_device, iv::NodeExecutor& executor, size_t index, size_t block_size)
    {
        audio_device.device().begin_requested_block(index, block_size);
        executor.tick_block(index, block_size);
        if (!audio_device.device().wait_until_block_ready()) {
            throw std::runtime_error("benchmark device block did not become ready");
        }
        audio_device.device().finish_requested_block();
    }
}

int main(int argc, char** argv)
{
    iv::test::install_crash_handlers();

    try {
        Options const options = parse_args(argc, argv);

        iv::test::FakeAudioDevice audio_device(
            iv::RenderConfig{
                .sample_rate = 48000,
                .num_channels = 2,
                .max_block_frames = 4096,
                .preferred_block_size = 256,
            }
        );

        iv::ModuleLoader loader = iv::test::make_loader();
        iv::ExecutionTargetRegistry execution_targets(iv::test::make_audio_device_provider(audio_device));
        iv::NodeExecutor executor = iv::test::make_executor(loader, audio_device, execution_targets, 1, options.module_path);

        size_t const block_size = options.block_size == 0 ? executor.max_block_size() : options.block_size;
        iv::validate_block_size(block_size, "benchmark block size must be a power of 2");
        if (block_size > executor.max_block_size()) {
            throw std::invalid_argument(
                "benchmark block size " + std::to_string(block_size) +
                " exceeds executor max block size " + std::to_string(executor.max_block_size())
            );
        }

        size_t index = 0;
        for (size_t i = 0; i < options.warmup_blocks; ++i) {
            request_tick_and_drain(audio_device, executor, index, block_size);
            index += block_size;
        }

        std::vector<double> samples_us;
        samples_us.reserve(options.min_measure_blocks + 32);

        using clock = std::chrono::steady_clock;
        auto const measure_begin = clock::now();
        while (true) {
            auto const tick_begin = clock::now();
            request_tick_and_drain(audio_device, executor, index, block_size);
            auto const tick_end = clock::now();
            index += block_size;

            double const elapsed_us = std::chrono::duration<double, std::micro>(tick_end - tick_begin).count();
            samples_us.push_back(elapsed_us);

            bool const enough_blocks = samples_us.size() >= options.min_measure_blocks;
            if (enough_blocks) {
                break;
            }
        }

        auto const measure_end = clock::now();
        double const total_elapsed_s = std::chrono::duration<double>(measure_end - measure_begin).count();
        uint64_t const measured_blocks = static_cast<uint64_t>(samples_us.size());
        uint64_t const measured_frames = measured_blocks * static_cast<uint64_t>(block_size);
        double const rendered_seconds = static_cast<double>(measured_frames) / static_cast<double>(audio_device.config().sample_rate);
        double const realtime_factor = rendered_seconds / total_elapsed_s;
        double const ns_per_frame = (total_elapsed_s * 1.0e9) / static_cast<double>(measured_frames);

        Stats const stats = compute_stats(samples_us);

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "module: " << std::filesystem::weakly_canonical(options.module_path).generic_string() << '\n';
        std::cout << "sample_rate: " << audio_device.config().sample_rate << '\n';
        std::cout << "block_size: " << block_size << '\n';
        std::cout << "executor_max_block_size: " << executor.max_block_size() << '\n';
        std::cout << "warmup_blocks: " << options.warmup_blocks << '\n';
        std::cout << "measured_blocks: " << measured_blocks << '\n';
        std::cout << "measured_frames: " << measured_frames << '\n';
        std::cout << "elapsed_s: " << total_elapsed_s << '\n';
        std::cout << "rendered_s: " << rendered_seconds << '\n';
        std::cout << "realtime_factor: " << realtime_factor << "x\n";
        std::cout << '\n';
        std::cout << "ns_per_frame: " << ns_per_frame << '\n';
        std::cout << "p95_us_per_block: " << stats.p95_us << '\n';
        std::cout << '\n';
        std::cout << "mean_us_per_block: " << stats.mean_us << '\n';
        std::cout << "median_us_per_block: " << stats.median_us << '\n';
        std::cout << "min_us_per_block: " << stats.min_us << '\n';
        std::cout << "max_us_per_block: " << stats.max_us << '\n';
        std::cout << "stddev_us_per_block: " << stats.stddev_us << '\n';

        return 0;
    } catch (std::exception const& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
