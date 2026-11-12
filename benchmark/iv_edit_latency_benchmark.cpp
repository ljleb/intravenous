// Edit-to-activation latency benchmark: the Front 16 priority target is at most
// 0.2 s from a project-graph edit to the executor accepting its recompiled
// generation (docs/implementation_fronts_direction.md, Front 16).
//
// Runs the server's project pipeline without a frontend, audio device, or RPC
// server. The package watcher builds the built-in package, and any package the
// shape needs, first; that time is excluded. Scripted edits then go through the same ProjectGraph handlers that
// RPC edits use. A realtime thread paced to the block period activates and ticks
// accepted generations the way the audio callback does, so every edit after the
// first compiles against an active predecessor, as in a live session.
//
// The synthetic project is one of several graph shapes of size N (--shape,
// --size), so that compilation and tick-scheduling changes are not tuned to one
// graph:
//   voices    N parallel identical chains (wide and shallow):
//               constant(frequency) -> saw_oscillator -> simple_iir_low_pass -> dummy_sink
//               constant(cutoff) ---------------------^
//   chain     one saw_oscillator through N simple_iir_low_pass in series (deep)
//   mixed     N voices alternating oscillator and filter types, each through a
//             warper, and each voice subtracting the previous voice's signal
//             (heterogeneous types and cross-voice fan-out)
//   feedback  N instances of iv.bench.feedback_voice, a module in a
//             benchmark-local package with a one-sample feedback loop,
//               saw -> subtract -> simple_iir_low_pass -> detach() -> subtract,
//             which forces sample-by-sample interleaving across node types.
//             Project connections cannot express feedback; only module code
//             can break a cycle with detach(), so the loop lives in a package.
//   storage   N instances of iv.bench.storage_voice. Each instance combines
//             retained sample history, mono-to-stereo conversion, sample
//             fan-out, event fan-in, event fan-out, and retained event history.
// Each iteration applies five edits: change the shape's frequency constant, add
// a sink, connect the shape's tap output to it, disconnect it, and delete the
// sink.
//
// Usage: iv_edit_latency_benchmark [--shape NAME] [--size N] [--iterations N]
//
// stderr receives ProjectGraph's per-edit "edit timing" breakdown
// (IV_EDIT_TIMING). stdout receives one line per edit and a per-kind summary of
// medians. Each edit line gives the new generation's tick_block() mean,
// nearest-rank p50, p95, and p99, and maximum per block over the settle
// window. --profile-seconds reports the same statistics for its longer window,
// with the mean, p99, and maximum as shares of the block period.

#include <intravenous/compat.h>
#include <intravenous/global_index.h>
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
#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/project_graph_graph_connections_bridge.h>
#include <intravenous/runtime/project_graph_graph_executor_bridge.h>
#include <intravenous/runtime/project_graph_graph_jit_bridge.h>
#include <intravenous/runtime/project_graph_node_instances_bridge.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/startup_config.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

constexpr std::string_view constant_id = "iv.builtin.constant";
constexpr std::string_view saw_id = "iv.builtin.saw_oscillator";
constexpr std::string_view sine_id = "iv.builtin.sine_oscillator";
constexpr std::string_view low_pass_id = "iv.builtin.simple_iir_low_pass";
constexpr std::string_view high_pass_id = "iv.builtin.simple_iir_high_pass";
constexpr std::string_view warper_id = "iv.builtin.warper";
constexpr std::string_view subtract_id = "iv.builtin.subtract";
constexpr std::string_view sink_id = "iv.builtin.dummy_sink";

struct Options {
    std::string shape = "voices";
    int size = 16;
    int iterations = 5;
    // Time the realtime thread runs each accepted generation after cutover;
    // its tick cost is measured over this window.
    int settle_ms = 300;
    // After the initial graph activates, run only that graph for this many
    // seconds. This gives a sampling profiler a stable audio-thread workload.
    int profile_seconds = 0;
    // Zero keeps the project execution configuration.
    std::size_t block_size = 0;
    iv::RealtimeStorageCostModel realtime_storage_cost_model{};
    std::optional<std::filesystem::path> results_tsv{};
    std::optional<std::filesystem::path> workspace{};
};

std::size_t parse_size(std::string_view option, std::string_view text)
{
    std::size_t result = 0;
    auto const [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error(
            "invalid non-negative integer for " + std::string(option));
    }
    return result;
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string_view const arg = argv[i];
        auto text_value = [&]() -> std::string_view {
            if (++i == argc) throw std::runtime_error("missing value for " + std::string(arg));
            return argv[i];
        };
        auto int_value = [&] {
            auto const text = text_value();
            std::size_t consumed = 0;
            auto const result = std::stoi(std::string(text), &consumed);
            if (consumed != text.size()) {
                throw std::runtime_error("invalid integer for " + std::string(arg));
            }
            return result;
        };
        if (arg == "--shape") {
            options.shape = text_value();
        } else if (arg == "--size") options.size = int_value();
        else if (arg == "--iterations") options.iterations = int_value();
        else if (arg == "--settle-ms") options.settle_ms = int_value();
        else if (arg == "--profile-seconds") options.profile_seconds = int_value();
        else if (arg == "--block-size") {
            options.block_size = parse_size(arg, text_value());
        } else if (arg == "--stack-budget-bytes") {
            options.realtime_storage_cost_model.stack_budget_bytes =
                parse_size(arg, text_value());
        } else if (arg == "--copied-byte-weight") {
            options.realtime_storage_cost_model.copied_byte_weight =
                parse_size(arg, text_value());
        } else if (arg == "--ring-addressed-byte-weight") {
            options.realtime_storage_cost_model.ring_addressed_byte_weight =
                parse_size(arg, text_value());
        } else if (arg == "--stack-footprint-byte-weight") {
            options.realtime_storage_cost_model.stack_footprint_byte_weight =
                parse_size(arg, text_value());
        } else if (arg == "--persistent-footprint-byte-weight") {
            options.realtime_storage_cost_model.persistent_footprint_byte_weight =
                parse_size(arg, text_value());
        } else if (arg == "--results-tsv") {
            options.results_tsv = std::filesystem::path(text_value());
        } else if (arg == "--workspace") {
            options.workspace = std::filesystem::path(text_value());
        } else if (arg == "--help") {
            std::cout << "Usage: iv_edit_latency_benchmark"
                         " [--shape voices|chain|mixed|feedback|storage] [--size N]"
                         " [--iterations N] [--settle-ms N]"
                         " [--profile-seconds N] [--block-size N]"
                         " [--stack-budget-bytes N]"
                         " [--copied-byte-weight N]"
                         " [--ring-addressed-byte-weight N]"
                         " [--stack-footprint-byte-weight N]"
                         " [--persistent-footprint-byte-weight N]"
                         " [--results-tsv PATH] [--workspace PATH]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown option " + std::string(arg));
        }
    }
    if (options.size < 1 || options.iterations < 1 || options.settle_ms < 0
        || options.profile_seconds < 0) {
        throw std::runtime_error(
            "--size and --iterations must be positive; timing values cannot be negative");
    }
    if (options.block_size != 0
        && (options.block_size & (options.block_size - 1)) != 0) {
        throw std::runtime_error("--block-size must be a power of two");
    }
    return options;
}

void write_text_if_changed(
    std::filesystem::path const& path,
    std::string_view contents)
{
    std::ifstream input(path, std::ios::binary);
    std::string existing{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (input && existing == contents) return;

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot write benchmark package file " + path.string());
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        throw std::runtime_error("cannot finish benchmark package file " + path.string());
    }
}

iv::ProjectNodePortMatcher port(std::string instance, std::string name)
{
    return {.instance_id = std::move(instance), .port = {.name = std::move(name)}};
}

iv::ProjectNodePortMatcher port(std::string instance, std::size_t index)
{
    return {.instance_id = std::move(instance), .port = {.index = index}};
}

iv::ProjectConnection sample_connection(
    std::string id, iv::ProjectNodePortMatcher source, iv::ProjectNodePortMatcher target)
{
    return iv::ProjectSampleConnection{
        .connection_id = std::move(id),
        .outputs = {std::move(source)},
        .inputs = {std::move(target)},
    };
}

// A project-local package the shape writes into the workspace before the package
// watcher starts: one directory holding a manifest and its module.cpp entry.
struct BenchmarkPackage {
    std::string directory{};
    std::string module_source{};
};

// One benchmark project: the graph, the constant whose value the configure edit
// changes, the output the connect edit attaches the extra sink to, the
// definitions the graph needs before the first edit, and the packages that
// provide definitions the built-in package lacks.
struct ShapeProject {
    iv::ProjectReplaceGraphRequest graph{};
    std::string configure_instance{};
    iv::ProjectNodePortMatcher tap{};
    std::vector<std::string_view> definitions{};
    std::vector<BenchmarkPackage> packages{};
};

struct GraphWriter {
    iv::ProjectReplaceGraphRequest graph{};

    void instance(std::string id, std::string_view definition, std::string source = {})
    {
        graph.instances.push_back(iv::ProjectNodeInstanceDeclaration{
            .instance_id = std::move(id),
            .definition_id = std::string(definition),
            .configuration_source = std::move(source),
        });
    }

    void connect(iv::ProjectNodePortMatcher source, iv::ProjectNodePortMatcher target)
    {
        auto id = "c" + std::to_string(graph.connections.size());
        graph.connections.push_back(
            sample_connection(std::move(id), std::move(source), std::move(target)));
    }
};

ShapeProject voices_project(int voices)
{
    GraphWriter g;
    for (int voice = 0; voice < voices; ++voice) {
        auto const v = std::to_string(voice);
        g.instance("frequency_" + v, constant_id, std::to_string(110 + 10 * voice));
        // log(1'200 / 20) / log(20'000 / 20)
        g.instance("cutoff_" + v, constant_id, "0.592675");
        g.instance("oscillator_" + v, saw_id);
        g.instance("filter_" + v, low_pass_id);
        g.instance("sink_" + v, sink_id);
        g.connect(port("frequency_" + v, 0), port("oscillator_" + v, "frequency"));
        g.connect(port("oscillator_" + v, "out"), port("filter_" + v, "in"));
        g.connect(port("cutoff_" + v, 0), port("filter_" + v, "cutoff"));
        g.connect(port("filter_" + v, "out"), port("sink_" + v, 0));
    }
    return {
        .graph = std::move(g.graph),
        .configure_instance = "frequency_0",
        .tap = port("oscillator_0", "out"),
        .definitions = {constant_id, saw_id, low_pass_id, sink_id},
    };
}

ShapeProject chain_project(int length)
{
    GraphWriter g;
    g.instance("frequency_0", constant_id, "220");
    g.instance("cutoff", constant_id, "0.5");
    g.instance("oscillator", saw_id);
    g.connect(port("frequency_0", 0), port("oscillator", "frequency"));
    auto previous = port("oscillator", "out");
    for (int stage = 0; stage < length; ++stage) {
        auto const id = "filter_" + std::to_string(stage);
        g.instance(id, low_pass_id);
        g.connect(previous, port(id, "in"));
        g.connect(port("cutoff", 0), port(id, "cutoff"));
        previous = port(id, "out");
    }
    g.instance("sink", sink_id);
    g.connect(previous, port("sink", 0));
    return {
        .graph = std::move(g.graph),
        .configure_instance = "frequency_0",
        .tap = port("filter_" + std::to_string(length / 2), "out"),
        .definitions = {constant_id, saw_id, low_pass_id, sink_id},
    };
}

ShapeProject mixed_project(int voices)
{
    GraphWriter g;
    for (int voice = 0; voice < voices; ++voice) {
        auto const v = std::to_string(voice);
        bool const even = voice % 2 == 0;
        g.instance("frequency_" + v, constant_id, std::to_string(110 + 10 * voice));
        g.instance("cutoff_" + v, constant_id, "0.5");
        g.instance("oscillator_" + v, even ? saw_id : sine_id);
        g.instance("filter_" + v, even ? low_pass_id : high_pass_id);
        g.instance("warper_" + v, warper_id);
        g.instance("difference_" + v, subtract_id);
        g.instance("sink_" + v, sink_id);
        g.connect(port("frequency_" + v, 0), port("oscillator_" + v, "frequency"));
        g.connect(port("oscillator_" + v, "out"), port("filter_" + v, "in"));
        g.connect(port("cutoff_" + v, 0), port("filter_" + v, "cutoff"));
        g.connect(port("filter_" + v, "out"), port("warper_" + v, "in"));
        g.connect(port("warper_" + v, "anti_aliased"), port("difference_" + v, 0));
        g.connect(
            voice == 0 ? port("cutoff_" + v, 0)
                       : port("warper_" + std::to_string(voice - 1), "anti_aliased"),
            port("difference_" + v, 1));
        g.connect(port("difference_" + v, 0), port("sink_" + v, 0));
    }
    return {
        .graph = std::move(g.graph),
        .configure_instance = "frequency_0",
        .tap = port("warper_0", "anti_aliased"),
        .definitions = {constant_id, saw_id, sine_id, low_pass_id, high_pass_id,
            warper_id, subtract_id, sink_id},
    };
}

constexpr std::string_view feedback_voice_id = "iv.bench.feedback_voice";
constexpr std::string_view storage_voice_id = "iv.bench.storage_voice";

// One feedback voice: a saw minus the voice's own low-pass output one sample
// earlier, low-pass filtered.
constexpr char const* feedback_voice_source = R"(#include <intravenous/dsl.h>

void feedback_voice(iv::GraphBuilder& g)
{
    auto frequency = g.input<"frequency">(110.0f);
    auto cutoff = g.input<"cutoff">(0.5f);
    auto oscillator = g.node<"iv.builtin.saw_oscillator">();
    oscillator.connect_input("frequency", frequency);
    auto filter = g.node<"iv.builtin.simple_iir_low_pass">();
    auto difference = g.node<"iv.builtin.subtract">();
    difference.connect_input(0, static_cast<iv::SamplePortRef>(oscillator));
    difference.connect_input(1, static_cast<iv::SamplePortRef>(filter).detach());
    filter.connect_input("in", static_cast<iv::SamplePortRef>(difference));
    filter.connect_input("cutoff", cutoff);
    g.outputs(filter);
}

IV_MODULE("iv.bench.feedback_voice", feedback_voice);
)";

ShapeProject feedback_project(int loops)
{
    GraphWriter g;
    for (int loop = 0; loop < loops; ++loop) {
        auto const v = std::to_string(loop);
        g.instance("frequency_" + v, constant_id, std::to_string(110 + 10 * loop));
        g.instance("cutoff_" + v, constant_id, "0.5");
        g.instance("voice_" + v, feedback_voice_id);
        g.instance("sink_" + v, sink_id);
        g.connect(port("frequency_" + v, 0), port("voice_" + v, "frequency"));
        g.connect(port("cutoff_" + v, 0), port("voice_" + v, "cutoff"));
        g.connect(port("voice_" + v, 0), port("sink_" + v, 0));
    }
    return {
        .graph = std::move(g.graph),
        .configure_instance = "frequency_0",
        .tap = port("voice_0", 0),
        .definitions = {constant_id, feedback_voice_id, sink_id},
        .packages = {{
            .directory = "feedback_voice",
            .module_source = feedback_voice_source,
        }},
    };
}

constexpr char const* storage_voice_source = R"(#include <intravenous/dsl.h>

struct StorageSampleSource {
    std::size_t history = 0;

    explicit StorageSampleSource(std::size_t history_ = 0)
        : history(history_)
    {}

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("drive")};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::tick)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<StorageSampleSource>& ctx) const
    {
        ctx.output<"out">().history() = history;
        ctx.output<"out">().latency() = 16;
    }

    void tick_block(iv::TickBlockContext<StorageSampleSource> const& ctx) const
    {
        auto input = ctx.input<"drive">();
        auto output = ctx.output<"out">();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(input.get_value(i));
        }
    }
};

struct StorageHistoryConsumer {
    struct State {
        float oldest = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in")};
    }

    static constexpr auto outputs() { return std::tuple{}; }

    void constrain_ports(
        iv::ConstrainPortsContext<StorageHistoryConsumer>& ctx) const
    {
        ctx.input<"in">().history() = 384;
    }

    void tick_block(iv::TickBlockContext<StorageHistoryConsumer> const& ctx) const
    {
        ctx.state().oldest = static_cast<float>(ctx.input<"in">().get(384));
    }
};

struct StorageStereoConsumer {
    struct State {
        float left = 0.0f;
        float right = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", {
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            },
        })};
    }

    static constexpr auto outputs() { return std::tuple{}; }

    void constrain_ports(
        iv::ConstrainPortsContext<StorageStereoConsumer>& ctx) const
    {
        ctx.input<"in">().history() = 96;
    }

    void tick_block(iv::TickBlockContext<StorageStereoConsumer> const& ctx) const
    {
        ctx.state().left = static_cast<float>(ctx.input<"in">().get(96, 0));
        ctx.state().right = static_cast<float>(ctx.input<"in">().get(96, 1));
    }
};

struct StorageEventSource {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            "events",
            iv::EventTypeId::trigger,
            iv::OutputProduction::tick,
            {.max_events_per_index = 0.25})};
    }

    void tick_block(iv::TickBlockContext<StorageEventSource> const& ctx) const
    {
        if (ctx.block_size != 0) {
            ctx.output<"events">().push(iv::TriggerEvent{}, ctx.block_size / 2);
        }
    }
};

struct StorageEventConsumer {
    struct State {
        std::size_t count = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::event_input(
            "events",
            iv::EventTypeId::trigger,
            iv::InputAccess::sequential)};
    }

    static constexpr auto outputs() { return std::tuple{}; }

    void constrain_ports(
        iv::ConstrainPortsContext<StorageEventConsumer>& ctx) const
    {
        ctx.input<"events">().history() = 256;
    }

    void tick_block(iv::TickBlockContext<StorageEventConsumer> const& ctx) const
    {
        ctx.state().count = ctx.input<"events">().events().size();
    }
};

void storage_voice(iv::GraphBuilder& graph)
{
    auto drive = graph.input<"drive">(0.25f);
    auto samples = graph.node<"iv.bench.storage_sample_source">(
        std::size_t{512});
    auto converted_samples = graph.node<"iv.bench.storage_sample_source">(
        std::size_t{64});
    auto history = graph.node<"iv.bench.storage_history_consumer">();
    auto stereo = graph.node<"iv.bench.storage_stereo_consumer">();
    samples(drive);
    converted_samples(drive);
    history(samples);
    stereo(converted_samples);

    auto events_a = graph.node<"iv.bench.storage_event_source">();
    auto events_b = graph.node<"iv.bench.storage_event_source">();
    std::array event_ports{events_a.event_port(), events_b.event_port()};
    auto merged = graph.make_event_port(event_ports);
    auto event_sink_a = graph.node<"iv.bench.storage_event_consumer">();
    auto event_sink_b = graph.node<"iv.bench.storage_event_consumer">();
    event_sink_a.connect_event_input(0, merged);
    event_sink_b.connect_event_input(0, merged);
    graph.outputs(samples);
}

IV_NODE("iv.bench.storage_sample_source", StorageSampleSource);
IV_NODE("iv.bench.storage_history_consumer", StorageHistoryConsumer);
IV_NODE("iv.bench.storage_stereo_consumer", StorageStereoConsumer);
IV_NODE("iv.bench.storage_event_source", StorageEventSource);
IV_NODE("iv.bench.storage_event_consumer", StorageEventConsumer);
IV_MODULE("iv.bench.storage_voice", storage_voice);
)";

ShapeProject storage_project(int voices)
{
    GraphWriter g;
    for (int voice = 0; voice < voices; ++voice) {
        auto const v = std::to_string(voice);
        g.instance("drive_" + v, constant_id, std::to_string(voice + 1));
        g.instance("voice_" + v, storage_voice_id);
        g.instance("sink_" + v, sink_id);
        g.connect(port("drive_" + v, 0), port("voice_" + v, "drive"));
        g.connect(port("voice_" + v, 0), port("sink_" + v, 0));
    }
    return {
        .graph = std::move(g.graph),
        .configure_instance = "drive_0",
        .tap = port("voice_0", 0),
        .definitions = {constant_id, storage_voice_id, sink_id},
        .packages = {{
            .directory = "storage_voice",
            .module_source = storage_voice_source,
        }},
    };
}

ShapeProject shape_project(std::string_view shape, int size)
{
    if (shape == "voices") return voices_project(size);
    if (shape == "chain") return chain_project(size);
    if (shape == "mixed") return mixed_project(size);
    if (shape == "feedback") return feedback_project(size);
    if (shape == "storage") return storage_project(size);
    throw std::runtime_error("unknown shape " + std::string(shape));
}

// The tick_block() durations of the blocks in one measurement window: their
// mean, nearest-rank p50, p95, and p99, and maximum.
struct TickCost {
    double mean_us = 0;
    double p50_us = 0;
    double p95_us = 0;
    double p99_us = 0;
    double max_us = 0;
    std::size_t blocks = 0;
    // Blocks of a window longer than the recording ring; the summary covers
    // the window's most recent blocks.
    std::size_t dropped_blocks = 0;
};

struct EditRecord {
    std::string kind{};
    double wall_ms = 0;
    // Realtime cost of the new generation over the settle window, and the
    // block period it must fit in.
    TickCost tick{};
    double block_period_us = 0;
    bool published = false;
    bool accepted = false;
    std::size_t bundles = 0;
    iv::ProjectGraphRebuildTimings rebuild{};
    std::vector<iv::GraphJitCompileTimings> compiles{};
};

double ms(std::uint64_t microseconds) { return static_cast<double>(microseconds) / 1000.0; }

double median(std::vector<double> values)
{
    if (values.empty()) return 0;
    std::ranges::sort(values);
    return values[values.size() / 2];
}

// Activates and ticks accepted generations at the block period, as the audio
// device callback does (SystemAudioDevices). The realtime thread records each
// block's tick_block() duration in a ring allocated before the thread starts,
// so it never allocates. The main thread summarizes the blocks recorded since
// its previous request.
class RealtimeLoop {
    // More than 90 minutes of blocks at the default block period.
    static constexpr std::size_t recorded_block_capacity = std::size_t{1} << 20;

    std::unique_ptr<std::atomic<std::uint32_t>[]> tick_ns_ =
        std::make_unique<std::atomic<std::uint32_t>[]>(recorded_block_capacity);
    // Blocks recorded since the thread started. Only the realtime thread
    // writes it; the release store publishes each recorded duration.
    std::atomic<std::uint64_t> recorded_{0};
    // Main thread only: the first block of the next measurement window.
    std::uint64_t window_begin_ = 0;
    std::chrono::nanoseconds period_{};
    std::jthread thread_;

public:
    RealtimeLoop(iv::GraphExecutor& executor, std::size_t sample_rate, std::size_t block_size)
        : period_(std::chrono::nanoseconds(
              static_cast<std::int64_t>(block_size) * 1'000'000'000LL
              / static_cast<std::int64_t>(sample_rate)))
        , thread_([this, &executor, block_size](std::stop_token stop) {
            std::int64_t cursor = 0;
            auto next = Clock::now();
            while (!stop.stop_requested()) {
                auto const index = iv::GlobalIndex::from_signed(cursor);
                (void)executor.activate_pending(index);
                if (executor.active_generation()) {
                    auto const begin = Clock::now();
                    executor.tick_block(index, block_size);
                    auto const elapsed = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            Clock::now() - begin).count());
                    auto const block = recorded_.load(std::memory_order_relaxed);
                    tick_ns_[block % recorded_block_capacity].store(
                        static_cast<std::uint32_t>(std::min<std::uint64_t>(
                            elapsed, std::numeric_limits<std::uint32_t>::max())),
                        std::memory_order_relaxed);
                    recorded_.store(block + 1, std::memory_order_release);
                }
                cursor += static_cast<std::int64_t>(block_size);
                next += period_;
                std::this_thread::sleep_until(next);
            }
        })
    {}

    [[nodiscard]] double block_period_us() const
    {
        return std::chrono::duration<double, std::micro>(period_).count();
    }

    // Summarizes the blocks recorded since the previous call and starts the
    // next window.
    TickCost take_tick_cost()
    {
        auto const end = recorded_.load(std::memory_order_acquire);
        auto begin = window_begin_;
        window_begin_ = end;
        TickCost cost{};
        if (end == begin) return cost;
        if (end - begin > recorded_block_capacity) {
            cost.dropped_blocks = static_cast<std::size_t>(
                end - begin - recorded_block_capacity);
            begin = end - recorded_block_capacity;
        }
        std::vector<std::uint32_t> blocks;
        blocks.reserve(static_cast<std::size_t>(end - begin));
        for (auto block = begin; block < end; ++block) {
            blocks.push_back(tick_ns_[block % recorded_block_capacity].load(
                std::memory_order_relaxed));
        }
        std::ranges::sort(blocks);
        auto const count = blocks.size();
        std::uint64_t sum = 0;
        for (auto const nanoseconds : blocks) sum += nanoseconds;
        // Nearest rank: the smallest duration that at least percent% of the
        // blocks do not exceed.
        auto const percentile = [&](std::size_t percent) {
            auto const rank = std::max<std::size_t>(1, (percent * count + 99) / 100);
            return static_cast<double>(blocks[rank - 1]) / 1000.0;
        };
        cost.mean_us = static_cast<double>(sum) / static_cast<double>(count) / 1000.0;
        cost.p50_us = percentile(50);
        cost.p95_us = percentile(95);
        cost.p99_us = percentile(99);
        cost.max_us = static_cast<double>(blocks.back()) / 1000.0;
        cost.blocks = count;
        return cost;
    }
};

void wait_for_definitions(
    iv::ProjectGraph const& project_graph,
    iv::PackageWatcherService const& package_watcher_service,
    std::vector<std::string_view> const& required)
{
    auto const deadline = Clock::now() + std::chrono::minutes(30);
    auto last_report = Clock::now();
    while (true) {
        auto const snapshot = project_graph.definitions_snapshot();
        if (snapshot && std::ranges::all_of(required, [&](std::string_view id) {
                return snapshot->by_id.contains(std::string(id));
            })) {
            return;
        }
        if (auto failure = package_watcher_service.failure_message()) {
            throw std::runtime_error(*failure);
        }
        if (Clock::now() > deadline) {
            throw std::runtime_error("package definitions did not load in 30 minutes");
        }
        if (Clock::now() - last_report > std::chrono::seconds(10)) {
            std::cout << "waiting for the package builds...\n" << std::flush;
            last_report = Clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void print_record(EditRecord const& record, int iteration)
{
    std::cout << std::fixed << std::setprecision(2)
              << "edit=" << record.kind << " iteration=" << iteration
              << " wall_ms=" << record.wall_ms;
    if (!record.published) {
        std::cout << " published=no\n";
        return;
    }
    std::cout << " bundles=" << record.bundles
              << " accepted=" << (record.accepted ? "yes" : "no")
              << " configure_ms="
              << ms(record.rebuild.node_instances_us + record.rebuild.graph_connections_us
                     + record.rebuild.finish_us)
              << " graph_jit_ms=" << ms(record.rebuild.graph_jit_us)
              << " stage_ms=" << ms(record.rebuild.executor_stage_us)
              << " compilations=" << record.compiles.size()
              << " tick_mean_us=" << record.tick.mean_us
              << " tick_p50_us=" << record.tick.p50_us
              << " tick_p95_us=" << record.tick.p95_us
              << " tick_p99_us=" << record.tick.p99_us
              << " tick_max_us=" << record.tick.max_us
              << " tick_blocks=" << record.tick.blocks
              << " block_period_us=" << record.block_period_us << '\n';
}

void write_tsv_header(std::ostream& output)
{
    output
        << "schema\tshape\tgraph_size\tblock_size\titeration\tedit"
           "\tpublished\taccepted\tbundles\twall_ms\tconfigure_ms"
           "\tgraph_jit_ms\tstage_ms\ttick_mean_us\ttick_p50_us"
           "\ttick_p95_us\ttick_p99_us\ttick_max_us\ttick_blocks"
           "\tblock_period_us\tstack_budget_bytes\tcopied_byte_weight"
           "\tring_addressed_byte_weight\tstack_footprint_byte_weight"
           "\tpersistent_footprint_byte_weight\tcompile_present"
           "\tcompile_index\tcompile_transition\tcompile_total_us"
           "\tcompile_capture_us\tcompile_plan_us"
           "\tcompile_transition_plan_us\tcompile_emit_us"
           "\tcompile_verify_us\tcompile_optimization_us"
           "\tcompile_materialization_us\tselection_signature"
           "\tselected_decisions"
           "\ttransient_stack_decisions"
           "\tstack_with_persistent_carry_decisions"
           "\tfull_node_storage_decisions\tselected_copied_bytes"
           "\tselected_ring_addressed_bytes\tselected_stack_bytes"
           "\tselected_persistent_bytes\tselected_weighted_cost"
           "\tpacked_root_stack_bytes"
           "\tsample_persistent_allocation_bytes"
           "\tevent_persistent_allocation_bytes\n";
}

void write_tsv_record(
    std::ostream& output,
    Options const& options,
    std::size_t block_size,
    EditRecord const& record,
    int iteration)
{
    auto const configure_us = record.rebuild.node_instances_us
        + record.rebuild.graph_connections_us + record.rebuild.finish_us;
    auto write = [&](iv::GraphJitCompileTimings const* compile,
                     std::size_t compile_index) {
        static iv::GraphJitCompileTimings const empty_compile{};
        auto const& measured = compile ? *compile : empty_compile;
        auto const& storage = measured.realtime_storage;
        output << std::setprecision(17)
               << "1\t" << options.shape
               << '\t' << options.size
               << '\t' << block_size
               << '\t' << iteration
               << '\t' << record.kind
               << '\t' << record.published
               << '\t' << record.accepted
               << '\t' << record.bundles
               << '\t' << record.wall_ms
               << '\t' << ms(configure_us)
               << '\t' << ms(record.rebuild.graph_jit_us)
               << '\t' << ms(record.rebuild.executor_stage_us)
               << '\t' << record.tick.mean_us
               << '\t' << record.tick.p50_us
               << '\t' << record.tick.p95_us
               << '\t' << record.tick.p99_us
               << '\t' << record.tick.max_us
               << '\t' << record.tick.blocks
               << '\t' << record.block_period_us
               << '\t' << options.realtime_storage_cost_model.stack_budget_bytes
               << '\t' << options.realtime_storage_cost_model.copied_byte_weight
               << '\t' << options.realtime_storage_cost_model.ring_addressed_byte_weight
               << '\t' << options.realtime_storage_cost_model.stack_footprint_byte_weight
               << '\t' << options.realtime_storage_cost_model.persistent_footprint_byte_weight
               << '\t' << (compile != nullptr)
               << '\t' << compile_index
               << '\t' << measured.transition
               << '\t' << measured.total_us
               << '\t' << measured.capture_us
               << '\t' << measured.lowering_plan_us
               << '\t' << measured.lowering_transition_us
               << '\t' << measured.lowering_emit_us
               << '\t' << measured.verify_us
               << '\t' << measured.optimization_us
               << '\t' << measured.materialization_us
               << '\t' << storage.selection_signature
               << '\t' << storage.selected_decisions
               << '\t' << storage.transient_stack_decisions
               << '\t' << storage.stack_with_persistent_carry_decisions
               << '\t' << storage.full_node_storage_decisions
               << '\t' << storage.selected_copied_bytes
               << '\t' << storage.selected_ring_addressed_bytes
               << '\t' << storage.selected_stack_bytes
               << '\t' << storage.selected_persistent_bytes
               << '\t' << storage.selected_weighted_cost
               << '\t' << storage.packed_root_stack_bytes
               << '\t' << storage.sample_persistent_allocation_bytes
               << '\t' << storage.event_persistent_allocation_bytes
               << '\n';
    };

    if (record.compiles.empty()) {
        write(nullptr, 0);
        return;
    }
    for (std::size_t index = 0; index < record.compiles.size(); ++index) {
        write(&record.compiles[index], index);
    }
}

void print_summary(std::vector<EditRecord> const& records, std::string const& label)
{
    std::map<std::string, std::vector<EditRecord const*>> by_kind;
    for (auto const& record : records) by_kind[record.kind].push_back(&record);

    std::cout << "\nsummary (" << label << "; medians, ms; target: wall <= 200)\n";
    for (auto const& [kind, entries] : by_kind) {
        auto collect = [&](auto&& field) {
            std::vector<double> values;
            for (auto const* entry : entries) {
                if (entry->published) values.push_back(field(*entry));
            }
            return median(std::move(values));
        };
        auto compile_phase = [&](bool transition, auto&& field) {
            std::vector<double> values;
            for (auto const* entry : entries) {
                for (auto const& compile : entry->compiles) {
                    if (compile.transition == transition) values.push_back(ms(field(compile)));
                }
            }
            return median(std::move(values));
        };
        // A generation whose compilation failed is published with diagnostics
        // but never accepted; its timings and ticks do not measure a graph.
        auto const accepted = std::ranges::count_if(entries, [](auto const* entry) {
            return entry->published && entry->accepted;
        });
        std::cout << std::fixed << std::setprecision(2)
                  << kind << " (" << entries.size() << ", accepted " << accepted
                  << "): wall "
                  << collect([](EditRecord const& r) { return r.wall_ms; })
                  << " = configure "
                  << collect([](EditRecord const& r) {
                         return ms(r.rebuild.node_instances_us + r.rebuild.graph_connections_us
                             + r.rebuild.finish_us);
                     })
                  << " + graph jit "
                  << collect([](EditRecord const& r) { return ms(r.rebuild.graph_jit_us); })
                  << " + stage "
                  << collect([](EditRecord const& r) { return ms(r.rebuild.executor_stage_us); })
                  << "; realtime tick mean "
                  << collect([](EditRecord const& r) { return r.tick.mean_us; })
                  << " us, p50 "
                  << collect([](EditRecord const& r) { return r.tick.p50_us; })
                  << " us, p95 "
                  << collect([](EditRecord const& r) { return r.tick.p95_us; })
                  << " us, p99 "
                  << collect([](EditRecord const& r) { return r.tick.p99_us; })
                  << " us, max "
                  << collect([](EditRecord const& r) { return r.tick.max_us; })
                  << " us per block\n";
        for (bool const transition : {false, true}) {
            auto const total = compile_phase(transition, [](auto const& c) { return c.total_us; });
            if (total == 0) continue;
            std::cout << "    " << (transition ? "transition" : "steady    ")
                      << " compile " << total
                      << " = capture " << compile_phase(transition, [](auto const& c) { return c.capture_us; })
                      << " + plan " << compile_phase(transition, [](auto const& c) { return c.lowering_plan_us; })
                      << " + transition plan " << compile_phase(transition, [](auto const& c) { return c.lowering_transition_us; })
                      << " + emit " << compile_phase(transition, [](auto const& c) { return c.lowering_emit_us; })
                      << " + verify " << compile_phase(transition, [](auto const& c) { return c.verify_us; })
                      << " + optimize " << compile_phase(transition, [](auto const& c) { return c.optimization_us; })
                      << " + codegen " << compile_phase(transition, [](auto const& c) { return c.materialization_us; })
                      << '\n';
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    try {
        iv::install_crash_handlers();
        auto const options = parse_options(argc, argv);
        std::ofstream results_tsv;
        if (options.results_tsv) {
            results_tsv.open(*options.results_tsv, std::ios::trunc);
            if (!results_tsv) {
                throw std::runtime_error(
                    "cannot open --results-tsv path "
                    + options.results_tsv->string());
            }
            write_tsv_header(results_tsv);
        }
        // The server build directory holds .intravenous_defaults, which names the
        // built-in package roots and the execution configuration.
        ::setenv("INTRAVENOUS_DIR", IV_EDIT_BENCHMARK_INSTALL_DIR, 0);
        ::setenv("IV_EDIT_TIMING", "1", 0);

        auto const workspace = options.workspace.value_or(
            std::filesystem::temp_directory_path()
                / "iv_edit_latency_benchmark_workspace");
        if (!options.workspace) std::filesystem::remove_all(workspace);
        std::filesystem::create_directories(workspace);

        iv::StartupConfig startup_config(workspace, std::filesystem::current_path());
        auto const startup = startup_config.initialize();
        auto const block_size = options.block_size == 0
            ? startup.execution.block_size
            : options.block_size;

        auto const& cost_model = options.realtime_storage_cost_model;
        std::cout << "realtime storage cost model: stack_budget_bytes="
                  << cost_model.stack_budget_bytes
                  << " copied_byte_weight=" << cost_model.copied_byte_weight
                  << " ring_addressed_byte_weight="
                  << cost_model.ring_addressed_byte_weight
                  << " stack_footprint_byte_weight="
                  << cost_model.stack_footprint_byte_weight
                  << " persistent_footprint_byte_weight="
                  << cost_model.persistent_footprint_byte_weight
                  << " block_size=" << block_size << '\n';

        iv::NodeInstances node_instances;
        iv::NodeDefinitions node_definitions;
        iv::ProjectGraph project_graph;
        iv::GraphConnections graph_connections;
        iv::GraphJit graph_jit(iv::GraphJitConfig{
            .sample_rate = startup.execution.sample_rate,
            .block_size = block_size,
            .realtime_storage_cost_model = cost_model,
        });
        iv::PackageWatcher package_watcher;
        iv::PackageJit package_jit(startup);
        iv::PackageDefinitions package_definitions(startup.workspace_root);
        iv::GraphExecutor graph_executor;
        iv::PackageWatcherService package_watcher_service(
            package_watcher, startup.workspace_root, startup.package_roots);

        auto definitions_project_graph_scope =
            iv::node_definitions_project_graph_bridge::bind(node_definitions, project_graph);
        auto project_graph_instances_scope =
            iv::project_graph_node_instances_bridge::bind(project_graph, node_instances);
        auto project_graph_connections_scope =
            iv::project_graph_graph_connections_bridge::bind(project_graph, graph_connections);
        auto project_graph_graph_jit_scope =
            iv::project_graph_graph_jit_bridge::bind(project_graph, graph_jit);
        auto project_graph_graph_executor_scope =
            iv::project_graph_graph_executor_bridge::bind(project_graph, graph_executor);
        auto watcher_service_scope =
            iv::package_watcher_service_bridge::bind(package_watcher_service, package_watcher);
        auto watcher_jit_scope =
            iv::package_watcher_package_jit_bridge::bind(package_watcher, package_jit);
        auto watcher_packages_scope =
            iv::package_watcher_package_definitions_bridge::bind(
                package_watcher, package_definitions);
        auto packages_definitions_scope =
            iv::package_definitions_node_definitions_bridge::bind(
                package_definitions, node_definitions);
        auto watcher_instances_scope =
            iv::package_watcher_node_instances_bridge::bind(package_watcher, node_instances);

        auto project = shape_project(options.shape, options.size);
        // Discovery runs when the service starts, so shape packages are written first.
        for (auto const& package : project.packages) {
            auto const root = workspace / package.directory;
            std::filesystem::create_directories(root);
            write_text_if_changed(
                root / "iv_package.json",
                "{\"schema\":2,\"entry\":\"module.cpp\"}\n");
            write_text_if_changed(root / "module.cpp", package.module_source);
        }
        std::cout << "building the packages (not timed)...\n" << std::flush;
        package_watcher_service.start();
        wait_for_definitions(
            project_graph, package_watcher_service, project.definitions);

        RealtimeLoop realtime(
            graph_executor, startup.execution.sample_rate, block_size);

        std::vector<EditRecord> records;
        auto run_edit = [&](std::string kind, int iteration, std::function<void()> const& apply) {
            auto const before = project_graph.current_generation();
            auto const begin = Clock::now();
            apply();
            auto const end = Clock::now();
            auto const after = project_graph.current_generation();
            EditRecord record{
                .kind = std::move(kind),
                .wall_ms = std::chrono::duration<double, std::milli>(end - begin).count(),
            };
            if (after && after != before) {
                record.published = true;
                record.accepted = after->graph_executor_accepted;
                record.bundles = after->graph ? after->graph->node_bundles.size() : 0;
                record.rebuild = after->rebuild_timings;
                record.compiles = after->graph_jit_timings;
                for (auto const& diagnostic : after->graph_jit_diagnostics) {
                    std::cout << "graph jit diagnostic: " << diagnostic.message << '\n';
                }
                // Let the realtime thread cut over before the next edit, as it
                // would between two user edits.
                auto const deadline = Clock::now() + std::chrono::seconds(5);
                while (record.accepted
                       && graph_executor.active_generation() != after->generation
                       && Clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                // Skip the cutover block itself, then measure the generation.
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                (void)realtime.take_tick_cost();
                std::this_thread::sleep_for(std::chrono::milliseconds(options.settle_ms));
                record.tick = realtime.take_tick_cost();
                record.block_period_us = realtime.block_period_us();
            } else if (auto const failure = project_graph.configuration_failure()) {
                for (auto const& diagnostic : failure->node_instance_diagnostics) {
                    std::cout << "configuration failure: " << diagnostic.instance_id << ": "
                              << diagnostic.message << '\n';
                }
            }
            print_record(record, iteration);
            if (results_tsv) {
                write_tsv_record(
                    results_tsv, options, block_size, record, iteration);
                results_tsv.flush();
            }
            records.push_back(std::move(record));
        };
        auto ack = [](auto&& handler) {
            iv::ProjectAckBuilder builder;
            handler(builder);
            builder.build();
        };

        run_edit("initial", 0, [&] {
            ack([&](auto& builder) {
                project_graph.handle_project_replace_graph(project.graph, builder);
            });
        });
        if (options.profile_seconds > 0) {
            if (!project_graph.current_generation()
                || !project_graph.current_generation()->graph_executor_accepted) {
                throw std::runtime_error(
                    "cannot profile because the initial graph was not accepted");
            }
            (void)realtime.take_tick_cost();
            std::cout << "profiling the accepted " << options.shape << " graph for "
                      << options.profile_seconds << " seconds...\n"
                      << std::flush;
            std::this_thread::sleep_for(
                std::chrono::seconds(options.profile_seconds));
            auto const cost = realtime.take_tick_cost();
            auto const period = realtime.block_period_us();
            auto const share = [&](double us) { return 100.0 * us / period; };
            std::cout << std::fixed << std::setprecision(2)
                      << "steady Tick: mean " << cost.mean_us << " us, p50 "
                      << cost.p50_us << " us, p95 " << cost.p95_us << " us, p99 "
                      << cost.p99_us << " us, max " << cost.max_us
                      << " us over " << cost.blocks << " blocks, block period "
                      << period << " us\n"
                      << "steady Tick share of the block period: mean "
                      << share(cost.mean_us) << "%, p99 " << share(cost.p99_us)
                      << "%, max " << share(cost.max_us) << "%\n";
            if (cost.dropped_blocks != 0) {
                std::cout << "steady Tick summary omits the first "
                          << cost.dropped_blocks
                          << " blocks, which exceed the recording ring\n";
            }
            package_watcher_service.request_shutdown();
            return 0;
        }
        for (int iteration = 0; iteration < options.iterations; ++iteration) {
            run_edit("configure", iteration, [&] {
                ack([&](auto& builder) {
                    project_graph.handle_project_update_node_instances(
                        iv::ProjectUpdateNodeInstancesRequest{.updates = {{
                            .instance_id = project.configure_instance,
                            .configuration_source = std::to_string(111 + iteration),
                        }}},
                        builder);
                });
            });
            run_edit("add_node", iteration, [&] {
                iv::ProjectStringBuilder builder;
                project_graph.handle_project_create_node_instance(
                    iv::ProjectCreateNodeInstanceRequest{
                        .instance_id = std::string("extra_sink"),
                        .definition_id = std::string(sink_id),
                    },
                    builder);
                (void)builder.build();
            });
            run_edit("connect", iteration, [&] {
                ack([&](auto& builder) {
                    project_graph.handle_project_upsert_graph_connection(
                        iv::ProjectUpsertGraphConnectionRequest{
                            .connection = sample_connection(
                                "extra", project.tap, port("extra_sink", 0)),
                        },
                        builder);
                });
            });
            run_edit("disconnect", iteration, [&] {
                ack([&](auto& builder) {
                    project_graph.handle_project_delete_graph_connection(
                        iv::ProjectDeleteGraphConnectionRequest{.connection_id = "extra"},
                        builder);
                });
            });
            run_edit("delete_node", iteration, [&] {
                ack([&](auto& builder) {
                    project_graph.handle_project_delete_node_instance(
                        iv::ProjectDeleteNodeInstanceRequest{.instance_id = "extra_sink"},
                        builder);
                });
            });
        }

        print_summary(records,
            "shape " + options.shape + ", size " + std::to_string(options.size) + ", "
                + std::to_string(project.graph.instances.size()) + " instances");
        package_watcher_service.request_shutdown();
        return 0;
    } catch (std::exception const& error) {
        std::cerr << "iv_edit_latency_benchmark: " << error.what() << '\n';
        return 1;
    }
}
