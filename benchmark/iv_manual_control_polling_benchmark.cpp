// Isolates the removed idle scan. Both cases execute the current executor on
// the same no-DSP graph; the legacy case adds the old all-binding polling loop.
// This measures polling overhead, not a prediction of a real DSP graph's gain.
#include <intravenous/compat.h>
#include <intravenous/execution/graph_executor.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::byte* current_storage = nullptr;

void tick(iv::graph_jit::RealtimeRootPlan const*, std::byte* storage,
    iv::graph_jit::TickInvocationCall const*, iv::GlobalIndex, std::size_t)
{
    current_storage = storage;
}

bool combine(iv::graph_jit::BackgroundPortIndex, std::byte*, std::byte const*, std::size_t)
{
    return true;
}

void initialize(std::span<std::byte> storage, std::span<std::byte const>)
{
    std::ranges::fill(storage, std::byte{});
}

struct LegacyBinding {
    std::shared_ptr<iv::ManualControlLiveTarget> target;
    std::size_t offset = 0;
    std::uint64_t observed_completion = 0;
};

IV_NOINLINE void poll(std::span<LegacyBinding> bindings)
{
    for (auto& binding : bindings) {
        auto const completion = binding.target->completion_sequence.load(std::memory_order_acquire);
        if (binding.target->open_gestures.load(std::memory_order_acquire) == 0
            && completion == binding.observed_completion) continue;
        std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(current_storage + binding.offset))
            .store(binding.target->target_bits.load(std::memory_order_relaxed), std::memory_order_relaxed);
        binding.observed_completion = completion;
    }
}

void measure(std::size_t count, std::size_t iterations, std::size_t trials)
{
    constexpr std::size_t block_size = 256;
    auto graph = std::make_shared<iv::CompiledGraph>();
    graph->project_generation = 1;
    graph->specialization.sample_rate = 48000;
    graph->specialization.block_size = block_size;
    graph->root_operations.tick_block = {.function = &tick};
    graph->root_operations.combine_sequential_input = &combine;
    iv::NodeLayoutBuilder builder(block_size);
    std::vector<iv::NodeLayout::RegionHandle> regions;
    for (std::size_t i = 0; i < count; ++i) {
        regions.push_back(builder.declare_raw_region(sizeof(iv::ManualControlRuntimeState),
            alignof(iv::ManualControlRuntimeState), "poll:" + std::to_string(i), &initialize));
    }
    graph->node_layout = std::move(builder).build();
    std::vector<LegacyBinding> legacy;
    for (std::size_t i = 0; i < count; ++i) {
        auto const offset = graph->node_layout.regions[regions[i].index].storage_offset;
        graph->manual_controls.push_back({
            .configured = {.id = {.node = {.instance_id = "node:" + std::to_string(i)}, .port_name = "value"},
                .input = {i, iv::PortKind::sample, 0}},
            .state_storage_offset = offset, .participates = true,
        });
        legacy.push_back({.target = std::make_shared<iv::ManualControlLiveTarget>(), .offset = offset});
    }
    iv::GraphExecutor executor;
    if (executor.stage(graph) != iv::GraphExecutorStageResult::accepted
        || !executor.activate_pending(iv::GlobalIndex{})) {
        throw std::runtime_error("could not activate polling benchmark graph");
    }
    std::uint64_t cursor = 0;
    auto run = [&](bool scan, std::size_t calls) {
        auto const bindings = scan ? std::span{legacy} : std::span<LegacyBinding>{};
        auto const start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < calls; ++i) {
            executor.tick_block(iv::GlobalIndex::from_bits(cursor), block_size);
            poll(bindings);
            cursor += block_size;
        }
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / calls;
    };
    run(false, 2048);
    run(true, 2048);
    std::vector<double> compact, scanned, saved;
    for (std::size_t trial = 0; trial < trials; ++trial) {
        double a, b;
        if (trial % 2 == 0) { a = run(false, iterations); b = run(true, iterations); }
        else { b = run(true, iterations); a = run(false, iterations); }
        compact.push_back(a); scanned.push_back(b); saved.push_back(b - a);
        std::cout << "trial\t" << count << '\t' << trial << '\t' << a << '\t' << b << '\t' << b - a << '\n';
    }
    auto median = [](std::vector<double> values) {
        std::ranges::sort(values);
        return (values[(values.size() - 1) / 2] + values[values.size() / 2]) * .5;
    };
    std::cout << "median\t" << count << "\t-\t" << median(compact) << '\t' << median(scanned)
        << '\t' << median(saved) << '\n';
}
} // namespace

int main(int argc, char** argv)
{
    try {
        std::size_t iterations = 100000, trials = 7;
        std::vector<std::size_t> counts{0, 1, 16, 256, 4096};
        for (int i = 1; i < argc; ++i) {
            std::string const option = argv[i];
            if (option == "--help") {
                std::cout << "iv_manual_control_polling_benchmark [--bindings N] [--iterations N] [--trials N]\n";
                return 0;
            }
            if ((option != "--bindings" && option != "--iterations" && option != "--trials") || i + 1 == argc)
                throw std::invalid_argument("unknown or incomplete option: " + option);
            std::string const argument = argv[++i];
            std::size_t end = 0;
            auto const value = std::stoull(argument, &end);
            if (end != argument.size() || argument.starts_with('-') || value > 10000000)
                throw std::invalid_argument("invalid benchmark count");
            if (option == "--bindings") {
                if (value > 100000) throw std::invalid_argument("binding count exceeds 100000");
                counts = {static_cast<std::size_t>(value)};
            } else if (option == "--iterations") iterations = value;
            else trials = value;
        }
        if (iterations == 0 || trials == 0) throw std::invalid_argument("iterations and trials must be positive");
        std::cout << std::fixed << std::setprecision(2)
            << "# No DSP; hot-cache idle polling isolation. Times are ns per executor Tick.\n"
            << "row\tbindings\ttrial\tcompact_ns\tlegacy_scan_ns\tsaved_ns\n";
        for (auto count : counts) measure(count, iterations, trials);
        return 0;
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
