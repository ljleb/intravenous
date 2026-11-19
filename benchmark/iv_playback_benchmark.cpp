#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/graph_executor.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/handlers.h>
#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/node_definitions_project_graph_bridge.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/package_definitions.h>
#include <intravenous/runtime/package_definitions_node_definitions_bridge.h>
#include <intravenous/runtime/package_jit.h>
#include <intravenous/runtime/package_watcher.h>
#include <intravenous/runtime/package_watcher_node_instances_bridge.h>
#include <intravenous/runtime/package_watcher_package_definitions_bridge.h>
#include <intravenous/runtime/package_watcher_package_jit_bridge.h>
#include <intravenous/runtime/package_watcher_service.h>
#include <intravenous/runtime/package_watcher_service_bridge.h>
#include <intravenous/runtime/playback_preparation_metrics.h>
#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/project_graph_graph_connections_bridge.h>
#include <intravenous/runtime/project_graph_graph_executor_bridge.h>
#include <intravenous/runtime/project_graph_graph_jit_bridge.h>
#include <intravenous/runtime/project_graph_node_instances_bridge.h>
#include <intravenous/runtime/project_graph_package_definitions_bridge.h>
#include <intravenous/runtime/project_graph_package_jit_bridge.h>
#include <intravenous/runtime/project_persistence.h>
#include <intravenous/runtime/project_persistence_package_jit_bridge.h>
#include <intravenous/runtime/project_persistence_project_graph_bridge.h>
#include <intravenous/runtime/startup_config.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Metrics = iv::PlaybackPreparationMetrics;
using Phase = iv::PlaybackPreparationPhase;

using SamplePreparation = Metrics::SamplePreparation;

struct Options {
    std::filesystem::path workspace = IV_PLAYBACK_BENCHMARK_WORKSPACE;
    std::optional<std::filesystem::path> trace_tsv;
    std::int64_t start_sample = -480000;
    std::optional<std::int64_t> seek_sample;
    int seconds = 60;
    int passes = 2;
    bool metrics = true;
    SamplePreparation sample_preparation = SamplePreparation::remap;
    std::size_t view_limit_kib = 0;
};

template<class Number>
Number integer(std::string_view option, std::string_view text)
{
    Number result{};
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::invalid_argument("invalid integer for " + std::string(option));
    return result;
}

Options parse(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string_view const option = argv[i];
        auto value = [&]() -> std::string_view {
            if (++i == argc) throw std::invalid_argument("missing value for " + std::string(option));
            return argv[i];
        };
        if (option == "--workspace") options.workspace = value();
        else if (option == "--start-sample") options.start_sample = integer<std::int64_t>(option, value());
        else if (option == "--seek-sample") options.seek_sample = integer<std::int64_t>(option, value());
        else if (option == "--seconds") options.seconds = integer<int>(option, value());
        else if (option == "--passes") options.passes = integer<int>(option, value());
        else if (option == "--trace-tsv") options.trace_tsv.emplace(value());
        else if (option == "--view-limit-kib") options.view_limit_kib = integer<std::size_t>(option, value());
        else if (option == "--sample-preparation") {
            auto const mode = value();
            if (mode == "remap") options.sample_preparation = SamplePreparation::remap;
            else if (mode == "reuse") options.sample_preparation = SamplePreparation::reuse;
            else if (mode == "pool") options.sample_preparation = SamplePreparation::pool;
            else if (mode == "arena") options.sample_preparation = SamplePreparation::arena;
            else throw std::invalid_argument("--sample-preparation must be remap, reuse, pool, or arena");
        }
        else if (option == "--no-metrics") options.metrics = false;
        else if (option == "--help") {
            std::cout << "Usage: iv_playback_benchmark [--workspace DIR] [--start-sample INDEX]"
                         " [--seek-sample INDEX] [--seconds N] [--passes N] [--trace-tsv PATH] [--no-metrics]\n"
                         " [--sample-preparation remap|reuse|pool|arena] [--view-limit-kib N]\n"
                         "By default, use the build-tree copy of the playback fixture.\n"
                         "Each pass runs for --seconds wall seconds. Later passes seek in the same graph.\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown option " + std::string(option));
        }
    }
    if (options.seconds <= 0 || options.passes <= 0)
        throw std::invalid_argument("--seconds and --passes must be positive");
    if (options.view_limit_kib > 1024 * 1024)
        throw std::invalid_argument("--view-limit-kib cannot exceed 1048576");
    options.workspace = std::filesystem::canonical(options.workspace);
    return options;
}

// Use the same stereo output node as the project. A render callback writes to
// this preallocated buffer instead of handing samples to a device.
struct OutputBuffer {
    std::vector<iv::Sample> samples;

    iv::ResourceContext resources() noexcept
    {
        return {.system_audio = {.owner = this, .resolve_output_fn = [](void* owner, std::string_view) {
            return iv::SystemAudioOutputBinding{owner, [](void* data) noexcept -> std::span<iv::Sample> {
                return static_cast<OutputBuffer*>(data)->samples;
            }};
        }}};
    }
};

struct PlaybackInterval {
    int pass = 0;
    double elapsed_s = 0;
    iv::ProjectPlaybackSnapshot playback{};
    std::uint64_t callbacks = 0;
    std::uint64_t underruns = 0;
    std::uint64_t missed_driver_periods = 0;
    double max_render_us = 0;
    float peak = 0;
    Metrics::Snapshot metrics{};
};

struct AudioDigest {
    std::uint64_t hash = 14695981039346656037ull;
    std::uint64_t samples = 0;

    void add(iv::Sample sample) noexcept
    {
        hash = (hash ^ std::bit_cast<std::uint32_t>(sample.value)) * 1099511628211ull;
        ++samples;
    }
};

Metrics::Snapshot difference(Metrics::Snapshot const& newer, Metrics::Snapshot const& older)
{
    auto delta = newer;
    for (std::size_t i = 0; i < delta.size(); ++i) {
        delta[i].calls -= older[i].calls;
        delta[i].total_ns -= older[i].total_ns;
        delta[i].work -= older[i].work;
    }
    return delta;
}

double milliseconds(Metrics::Snapshot const& metrics, Phase phase)
{
    return static_cast<double>(metrics[static_cast<std::size_t>(phase)].total_ns) / 1'000'000;
}

void wait_for_definitions(iv::ProjectGraph const& graph, iv::PackageWatcherService const& watcher)
{
    auto const deadline = Clock::now() + 5min;
    while (Clock::now() < deadline) {
        auto const definitions = graph.definitions_snapshot();
        if (definitions && definitions->by_id.contains("iv.builtin.automation.curve")
            && definitions->by_id.contains("iv.bench.playback.saw")
            && definitions->by_id.contains("iv.builtin.system_audio_output")) return;
        if (auto failure = watcher.failure_message()) throw std::runtime_error(*failure);
        std::this_thread::sleep_for(50ms);
    }
    throw std::runtime_error("the two-curve project's package definitions did not load");
}

void print_trace(std::vector<PlaybackInterval> const& intervals,
    std::vector<double> const& seek_ms, Metrics::Snapshot const& totals, std::ostream* tsv)
{
    if (tsv) {
        *tsv << "pass\telapsed_s\tproject_sample\tcallbacks\tunderruns\tmissed_driver_periods"
                "\tmax_render_us\tpeak";
        for (auto phase : Metrics::phase_names)
            *tsv << '\t' << phase << "_calls\t" << phase << "_ns\t" << phase << "_work";
        *tsv << '\n';
    }
    std::cout << std::fixed << std::setprecision(2);
    for (std::size_t pass = 0; pass < seek_ms.size(); ++pass)
        std::cout << "pass=" << pass + 1 << " seek_applied_ms=" << seek_ms[pass] << '\n';
    for (auto const& interval : intervals) {
        auto const position = interval.playback.background_playhead.signed_value();
        auto const& metrics = interval.metrics;
        std::cout << "pass=" << interval.pass << " elapsed_s=" << interval.elapsed_s
                  << " project_sample=" << position << " callbacks=" << interval.callbacks
                  << " underruns=" << interval.underruns
                  << " missed_driver_periods=" << interval.missed_driver_periods
                  << " render_max_us=" << interval.max_render_us << " peak=" << interval.peak
                  << " tock_ms=" << milliseconds(metrics, Phase::evaluate_tock)
                  << " cache_store_ms=" << milliseconds(metrics, Phase::store_output)
                  << " mapping_scan_ms=" << milliseconds(metrics, Phase::mapping_vma_check)
                  << " residency_scan_ms=" << milliseconds(metrics, Phase::residency_vma_check)
                  << '\n';
        if (!tsv) continue;
        *tsv << interval.pass << '\t' << interval.elapsed_s << '\t' << position
             << '\t' << interval.callbacks << '\t' << interval.underruns
             << '\t' << interval.missed_driver_periods << '\t' << interval.max_render_us
             << '\t' << interval.peak;
        for (auto const& phase : metrics)
            *tsv << '\t' << phase.calls << '\t' << phase.total_ns << '\t' << phase.work;
        *tsv << '\n';
    }
    for (std::size_t i = 0; i < totals.size(); ++i) {
        auto const& phase = totals[i];
        if (phase.calls == 0) continue;
        std::cout << "phase=" << Metrics::phase_names[i] << " calls=" << phase.calls
                  << " total_ms=" << static_cast<double>(phase.total_ns) / 1'000'000
                  << " max_us=" << static_cast<double>(phase.max_ns) / 1000
                  << " work=" << phase.work << '\n';
    }
}

void run(Options const& options)
{
    if (!std::filesystem::is_regular_file(options.workspace / "iv_project.jsonl"))
        throw std::runtime_error("workspace does not contain iv_project.jsonl");
    std::ofstream tsv;
    if (options.trace_tsv) {
        tsv.open(*options.trace_tsv, std::ios::out | std::ios::noreplace);
        if (!tsv) throw std::runtime_error("cannot create trace TSV " + options.trace_tsv->string());
    }
    ::setenv("INTRAVENOUS_DIR", IV_PLAYBACK_BENCHMARK_INSTALL_DIR, 0);
    auto const startup = iv::StartupConfig(options.workspace, options.workspace).initialize();
    auto const [sample_rate, block_size] = startup.execution;
    if (sample_rate == 0 || block_size == 0)
        throw std::runtime_error("playback configuration requires a sample rate and block size");
    auto const period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(static_cast<double>(block_size) / static_cast<double>(sample_rate)));
    if (period <= Clock::duration::zero()) throw std::runtime_error("playback block period is too short");

    Metrics metrics{options.sample_preparation};
    OutputBuffer output{std::vector<iv::Sample>(block_size * 2)};
    iv::NodeInstances instances;
    iv::NodeDefinitions definitions;
    iv::ProjectGraph project(sample_rate);
    iv::GraphConnections connections;
    iv::GraphJit jit(iv::GraphJitConfig{.sample_rate = sample_rate, .block_size = block_size});
    iv::BufferedStorageLimits storage_limits;
    if (options.view_limit_kib) storage_limits.max_contiguous_mapping_bytes = options.view_limit_kib * 1024;
    iv::GraphExecutor executor(output.resources(), {}, storage_limits, &metrics);
    iv::PackageWatcher watcher;
    iv::PackageJit package_jit(startup);
    iv::PackageDefinitions package_definitions(startup.workspace_root);
    iv::PackageWatcherService service(watcher, startup.workspace_root, startup.package_roots);
    iv::ProjectPersistence persistence(startup.workspace_root, startup);

    auto definitions_graph = iv::node_definitions_project_graph_bridge::bind(definitions, project);
    auto graph_instances = iv::project_graph_node_instances_bridge::bind(project, instances);
    auto graph_packages = iv::project_graph_package_definitions_bridge::bind(project, package_definitions);
    auto graph_toolchain = iv::project_graph_package_jit_bridge::bind(project, package_jit);
    auto graph_connections = iv::project_graph_graph_connections_bridge::bind(project, connections);
    auto graph_jit = iv::project_graph_graph_jit_bridge::bind(project, jit);
    auto graph_executor = iv::project_graph_graph_executor_bridge::bind(project, executor);
    auto watcher_service = iv::package_watcher_service_bridge::bind(service, watcher);
    auto watcher_jit = iv::package_watcher_package_jit_bridge::bind(watcher, package_jit);
    auto watcher_packages = iv::package_watcher_package_definitions_bridge::bind(watcher, package_definitions);
    auto packages_definitions = iv::package_definitions_node_definitions_bridge::bind(package_definitions, definitions);
    auto watcher_instances = iv::package_watcher_node_instances_bridge::bind(watcher, instances);
    auto persistence_project = iv::project_persistence_project_graph_bridge::bind(persistence, project);
    auto persistence_jit = iv::project_persistence_package_jit_bridge::bind(persistence, package_jit);
    struct StopCompilation {
        iv::PackageWatcherService& service;
        iv::ProjectGraph& project;
        ~StopCompilation() { service.stop(); project.stop_compilation(); }
    } stop_compilation{service, project};

    std::cout << "Loading playback fixture " << options.workspace << " before timing.\n" << std::flush;
    service.start();
    wait_for_definitions(project, service);
    service.stop();
    for (auto const& message : persistence.load()) {
        std::cerr << "project replay " << message.level << " " << message.message << '\n';
        if (message.level == "error") throw std::runtime_error("project replay failed");
    }
    if (!project.wait_for_compilation(5min)) throw std::runtime_error("project compilation timed out");
    if (auto const failure = project.configuration_failure()) {
        for (auto const& diagnostic : failure->node_instance_diagnostics)
            std::cerr << diagnostic.instance_id << " " << diagnostic.message << '\n';
        throw std::runtime_error("project configuration failed");
    }
    auto const accepted = executor.accepted_graph();
    if (!accepted || accepted->background_evaluation_plan.tick_sequential_inputs.empty()) {
        if (auto const generation = project.current_generation())
            for (auto const& diagnostic : generation->graph_jit_diagnostics)
                std::cerr << diagnostic.message << '\n';
        throw std::runtime_error("no accepted executable with background-supplied Sequential inputs");
    }

    auto render = [&] {
        std::ranges::fill(output.samples, iv::Sample{});
        executor.render_block(block_size, static_cast<double>(sample_rate));
    };
    auto const startup_deadline = Clock::now() + 15s;
    while (!executor.active_generation()) {
        if (Clock::now() >= startup_deadline || executor.has_failed_pending_generation())
            throw std::runtime_error("the initial playback generation did not activate");
        render();
        std::this_thread::sleep_for(period);
    }
    std::cout << "Playback sample_rate=" << sample_rate << " block_size=" << block_size
              << " background_tick_inputs=" << accepted->background_evaluation_plan.tick_sequential_inputs.size()
              << " seconds_per_pass=" << options.seconds << " passes=" << options.passes
              << " metrics=" << options.metrics << '\n' << std::flush;

    std::vector<PlaybackInterval> intervals;
    intervals.reserve(static_cast<std::size_t>(options.passes) * (static_cast<std::size_t>(options.seconds) + 1));
    std::vector<double> seek_ms;
    std::vector<AudioDigest> audio(static_cast<std::size_t>(options.passes));
    seek_ms.reserve(static_cast<std::size_t>(options.passes));
    if (options.metrics) metrics.enable();
    (void)executor.set_playback_running(true);
    auto previous = metrics.snapshot();
    auto previous_underruns = executor.playback_underrun_blocks();
    for (int pass = 0; pass < options.passes; ++pass) {
        auto const seek = executor.seek_playback(iv::GlobalIndex::from_signed(
            pass == 0 ? options.start_sample : options.seek_sample.value_or(options.start_sample)));
        auto const start = Clock::now();
        auto const end = start + std::chrono::seconds(options.seconds);
        auto next = start;
        auto report_at = start + 1s;
        seek_ms.push_back(-1);
        PlaybackInterval interval{.pass = pass + 1};
        auto report = [&] {
            interval.elapsed_s = std::chrono::duration<double>(Clock::now() - start).count();
            interval.playback = executor.playback_snapshot();
            auto const underruns = executor.playback_underrun_blocks();
            interval.underruns = underruns - previous_underruns;
            previous_underruns = underruns;
            auto const current = metrics.snapshot();
            interval.metrics = difference(current, previous);
            previous = current;
            intervals.push_back(interval);
            interval = {.pass = pass + 1};
        };
        while (next < end) {
            std::this_thread::sleep_until(next);
            auto const wake = Clock::now();
            if (wake >= end) break;
            auto const missed = (wake - next) / period;
            interval.missed_driver_periods += static_cast<std::uint64_t>(missed);
            next += period * (missed + 1);
            render();
            auto const rendered = Clock::now();
            ++interval.callbacks;
            interval.max_render_us = std::max(interval.max_render_us,
                std::chrono::duration<double, std::micro>(rendered - wake).count());
            if (seek_ms.back() < 0 && executor.playback_snapshot().applied_command >= seek)
                seek_ms.back() = std::chrono::duration<double, std::milli>(rendered - start).count();
            for (auto const sample : output.samples) {
                interval.peak = std::max(interval.peak, std::abs(sample.value));
                if (seek_ms.back() >= 0) audio[static_cast<std::size_t>(pass)].add(sample);
            }
            if (rendered >= report_at) {
                report();
                report_at += 1s;
            }
        }
        if (interval.callbacks || interval.missed_driver_periods) report();
    }

    auto const totals = metrics.snapshot();
    print_trace(intervals, seek_ms, totals, options.trace_tsv ? &tsv : nullptr);
    if (options.metrics)
        std::cout << "transaction_p99_upper_us=" << metrics.transaction_p99_upper_ns() / 1000 << '\n';
    for (std::size_t i = 0; i < audio.size(); ++i)
        std::cout << "pass=" << i + 1 << " audio_samples=" << audio[i].samples
                  << " audio_digest=" << std::hex << audio[i].hash << std::dec << '\n';
    auto const usage = executor.buffered_storage_usage();
    std::cout << "storage live_page_bytes=" << usage.live_page_bytes
              << " mapped_bytes=" << usage.reserved_mapping_bytes
              << " mapped_runs=" << usage.mapped_runs << " resident_bytes=" << usage.resident_bytes << '\n';
    if (tsv.is_open()) {
        tsv.close();
        if (!tsv) throw std::runtime_error("could not write the trace TSV");
    }
    if (executor.background_execution_failures().any() || std::ranges::any_of(seek_ms, [](double ms) { return ms < 0; }))
        throw std::runtime_error("background execution failed or a seek did not apply");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        iv::install_crash_handlers();
        run(parse(argc, argv));
    } catch (std::exception const& error) {
        std::cerr << "iv_playback_benchmark " << error.what() << '\n';
        return 1;
    }
    return 0;
}
