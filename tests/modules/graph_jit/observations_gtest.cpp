#include "test_support.h"
#include <intravenous/graph_jit/realtime/root_plan.h>
#include <intravenous/observations/observation_resolution.h>
#include <intravenous/rpc/socket_rpc_response_builders.h>
#include <intravenous/project/runtime_project_events.h>
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <iterator>
#include <limits>
#include <thread>

namespace iv::test::graph_jit_tests {
namespace {
std::vector<NodeBundleHandle> concrete_bundles(ConfiguredGraph const& graph)
{
    std::vector<NodeBundleHandle> result;
    for (NodeBundleHandle bundle = 0; bundle < graph.node_bundles.size(); ++bundle)
        if (graph.node_bundles.bundle(bundle).is_concrete()) result.push_back(bundle);
    return result;
}

bool wait_for_project_compilation(ProjectGraph& project, GraphExecutor& executor, GlobalIndex position)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        (void)executor.activate_pending(position);
        if (auto generation = executor.active_generation()) project.acknowledge_execution(*generation);
        if (project.wait_for_compilation(std::chrono::milliseconds(1))) return true;
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
}

TEST_F(GraphJitRuntimeFixture, LiveOutputObservationUsesTheProducedWindowWithoutChangingDsp)
{
    auto graph = configured_module_graph(*revision, "iv.test.graph_jit.state_context.transient_sample_module");
    auto const bundles = concrete_bundles(*graph);
    ASSERT_GE(bundles.size(), 2u);
    auto const& operation = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations.front();
    auto compiled = jit->compile(GraphJitCompileRequest{
        .project_generation = 701, .graph = graph, .definitions = definitions,
        .observations = {.revision = 9, .probes = {{.id = 17, .operation = &operation,
            .kind = ObservationSourceKind::output, .bundle = bundles.front(), .port = 0, .available = true}}},
    });
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(3)));
    executor.tick_block(index(3), 32);
    auto const* frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->count, 1u);
    EXPECT_EQ(frame->requirements_revision, 9u);
    EXPECT_EQ(frame->values[0].value.load<observation_details::SampleLevelState>().samples, 32u);
    EXPECT_DOUBLE_EQ(frame->values[0].value.load<observation_details::SampleLevelState>().peak, 34.0);
    EXPECT_FALSE(executor.background_execution_failures().any());
}

TEST_F(GraphJitRuntimeFixture, ForegroundHoldClearsLiveObservationsUntilTickResumes)
{
    auto graph = configured_module_graph(*revision, "iv.test.graph_jit.state_context.transient_sample_module");
    ASSERT_NE(graph, nullptr);
    auto const bundles = concrete_bundles(*graph);
    ASSERT_FALSE(bundles.empty());
    auto const& operation = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations.front();
    auto compiled = jit->compile(GraphJitCompileRequest{
        .project_generation = 702, .graph = graph, .definitions = definitions,
        .observations = {.revision = 9, .probes = {{.id = 17, .operation = &operation,
            .kind = ObservationSourceKind::output, .bundle = bundles.front(), .port = 0, .available = true}}},
    });
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    ProjectPlaybackRequest toggle{.operation = ProjectPlaybackOperation::toggle_foreground};
    executor.handle_playback(toggle);
    executor.render_block(32, 48000);
    auto const* frame = executor.collect_observations();
    ASSERT_TRUE(frame && frame->count == 1 && frame->values[0].value.captured);

    executor.handle_playback(toggle);
    executor.render_block(32, 48000);
    frame = executor.collect_observations();
    ASSERT_TRUE(frame && frame->count == 1);
    EXPECT_EQ(frame->sample_index, index(32));
    EXPECT_EQ(frame->block_size, 0u);
    EXPECT_FALSE(frame->values[0].value.captured);
    executor.render_block(32, 48000);
    EXPECT_EQ(executor.collect_observations(), nullptr);

    (void)executor.seek_playback(index(20));
    executor.render_block(32, 48000);
    EXPECT_EQ(executor.collect_observations(), nullptr);
    EXPECT_EQ(executor.playback_snapshot().background_playhead, index(20));
    EXPECT_EQ(executor.playback_snapshot().foreground_playhead, index(32));

    executor.handle_playback(toggle);
    executor.render_block(32, 48000);
    frame = executor.collect_observations();
    ASSERT_TRUE(frame && frame->count == 1 && frame->values[0].value.captured);
    EXPECT_EQ(frame->sample_index, index(32));
    EXPECT_EQ(frame->block_size, 32u);
    EXPECT_EQ(executor.playback_underrun_blocks(), 0u);
}

TEST_F(GraphJitRuntimeFixture, EarlyTickLeavesForegroundManualStateUntouched)
{
    auto graph = compile_with_manual_input("iv.test.graph_jit.early_tick_audio_mix", 703,
        "in", Sample{.25f});
    ASSERT_TRUE(graph.succeeded()) << (graph.diagnostics.empty() ? "" : graph.diagnostics.front().message);
    auto const& compiled = *graph.compiled_graph;
    auto const* root = compiled.root_operations.tick_block.plan.get();
    ASSERT_TRUE(graph_jit::has_early_tick(root));
    ASSERT_EQ(compiled.manual_controls.size(), 1u);
    auto storage = compiled.node_layout.create_storage(resources);
    storage.initialize();
    auto* state = reinterpret_cast<ManualControlRuntimeState*>(
        storage.buffer().data() + compiled.manual_controls.front().state_storage_offset);
    auto const preceding_value = std::bit_cast<std::uint32_t>(.75f);
    state->effective_bits = preceding_value;

    graph_jit::EarlyTickCursor cursor;
    cursor.reserve(root);
    graph_jit::TickTransferBuffer transfer_buffer;
    transfer_buffer.reserve(root);
    auto const transfer = transfer_buffer.view(*root);
    graph_jit::TickInvocationCall call{};
    graph_jit::execute_early_tick_root(*root, storage.buffer().data(), &call, index(0), 16, cursor, transfer);
    EXPECT_EQ(state->effective_bits, preceding_value);
    graph_jit::execute_foreground_tick_root(root, storage.buffer().data(), &call, index(0), 16, transfer);
    EXPECT_EQ(state->effective_bits, std::bit_cast<std::uint32_t>(.25f));
}

TEST_F(GraphJitRuntimeFixture, EarlyTickKeepsTwoReservedSampleBlocksInDeviceOrder)
{
    auto graph = compile("iv.test.graph_jit.early_tick_audio_mix", 703);
    ASSERT_TRUE(graph.succeeded()) << (graph.diagnostics.empty() ? "" : graph.diagnostics.front().message);
    auto const& compiled = *graph.compiled_graph;
    auto const* root = compiled.root_operations.tick_block.plan.get();
    ASSERT_TRUE(graph_jit::has_early_tick(root));
    std::array<Sample, 8> output{};
    resources.system_audio = {
        .owner = &output,
        .resolve_output_fn = [](void* owner, std::string_view) {
            return SystemAudioOutputBinding{owner, [](void* owner) noexcept -> std::span<Sample> {
                return *static_cast<std::array<Sample, 8>*>(owner);
            }};
        },
    };
    auto storage = compiled.node_layout.create_storage(resources);
    storage.initialize();
    graph_jit::EarlyTickCursor cursor;
    cursor.reserve(root);
    graph_jit::TickTransferBuffer first_buffer;
    first_buffer.reserve(root);
    graph_jit::TickTransferBuffer second_buffer;
    second_buffer.reserve(root);
    auto const first = first_buffer.view(*root);
    auto const second = second_buffer.view(*root);
    ASSERT_FALSE(first.empty());
    graph_jit::TickInvocationCall call{};
    graph_jit::execute_early_tick_root(*root, storage.buffer().data(), &call, index(20), 4, cursor, first);
    graph_jit::execute_early_tick_root(*root, storage.buffer().data(), &call, index(40), 4, cursor, second);
    EXPECT_EQ(output.front().value, 0.f);

    graph_jit::execute_foreground_tick_root(root, storage.buffer().data(), &call, index(20), 4, first);
    EXPECT_EQ(output.front().value, 21.f);
    EXPECT_EQ(output.back().value, 27.f);
    output.fill({});
    graph_jit::execute_foreground_tick_root(root, storage.buffer().data(), &call, index(40), 4, second);
    EXPECT_EQ(output.front().value, 45.f);
    EXPECT_EQ(output.back().value, 51.f);
}

TEST_F(GraphJitRuntimeFixture, RetainedSampleRingHoldsTwoRangesUntilDevicePlayback)
{
    auto graph = compile("iv.test.graph_jit.retained_early_tick_audio", 703);
    ASSERT_TRUE(graph.succeeded()) << (graph.diagnostics.empty() ? "" : graph.diagnostics.front().message);
    auto const& compiled = *graph.compiled_graph;
    auto const* root = compiled.root_operations.tick_block.plan.get();
    ASSERT_NE(root, nullptr);
    auto const& allocations = root->sample_ports.storage.persistent_allocations;
    ASSERT_TRUE(std::ranges::any_of(allocations, [](auto const& allocation) {
        return allocation.kind == graph_jit::detail::SamplePersistentStorageKind::ring
            && allocation.retained_frames == 129 && allocation.frame_capacity == 256;
    }));
    EXPECT_EQ(graph_jit::independent_early_frame_limit(root), 127u);

    std::array<Sample, 128> output{};
    resources.system_audio = {
        .owner = &output,
        .resolve_output_fn = [](void* owner, std::string_view) {
            return SystemAudioOutputBinding{owner, [](void* owner) noexcept -> std::span<Sample> {
                return *static_cast<std::array<Sample, 128>*>(owner);
            }};
        },
    };
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(graph.compiled_graph), GraphExecutorStageResult::accepted);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    PlaybackRenderSlice slice;
    do {
        executor.render_block(64, 88200, &slice, true, 64);
        ASSERT_FALSE(slice.rendered);
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::yield();
    } while (executor.playback_render_frames(128) != 64);
    (void)executor.set_playback_running(true);
    do {
        executor.render_block(64, 88200, &slice, true, 64);
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        if (!slice.rendered) std::this_thread::yield();
    } while (!slice.rendered);
    EXPECT_EQ(slice.foreground_sample, index(0));
    EXPECT_FLOAT_EQ(output.front().value, 1.f);
    EXPECT_FLOAT_EQ(output.back().value, 127.f);
    ASSERT_EQ(executor.playback_render_frames(128), 63u);
    output.fill({});
    executor.render_block(63, 88200, &slice);
    EXPECT_TRUE(slice.rendered);
    EXPECT_EQ(slice.foreground_sample, index(64));
    EXPECT_FLOAT_EQ(output.front().value, 129.f);
    EXPECT_FLOAT_EQ(output[125].value, 253.f);
    EXPECT_FLOAT_EQ(output[126].value, 0.f);
}

TEST_F(GraphJitRuntimeFixture, DevicePrerollDrainsTwoPreparedRangesBeforeApplyingASeek)
{
    auto graph = compile("iv.test.graph_jit.early_tick_audio_mix", 703);
    ASSERT_TRUE(graph.succeeded()) << (graph.diagnostics.empty() ? "" : graph.diagnostics.front().message);
    auto const& compiled = *graph.compiled_graph;
    ASSERT_GE(graph_jit::independent_early_frame_limit(compiled.root_operations.tick_block.plan.get()), 8u);
    struct AudioBuffers {
        std::array<Sample, 8> input{};
        std::array<Sample, 8> output{};
        std::atomic<unsigned> early_calls{0};
    } buffers;
    resources.system_audio = {
        .owner = &buffers,
        .resolve_input_fn = [](void* owner, std::string_view id) {
            if (id == "early_tick_test_gate") {
                return SystemAudioInputBinding{owner, [](void* owner) noexcept -> std::span<Sample const> {
                    static_cast<AudioBuffers*>(owner)->early_calls.fetch_add(1, std::memory_order_release);
                    return {};
                }};
            }
            return SystemAudioInputBinding{owner, [](void* owner) noexcept -> std::span<Sample const> {
                return static_cast<AudioBuffers*>(owner)->input;
            }};
        },
        .resolve_output_fn = [](void* owner, std::string_view) {
            return SystemAudioOutputBinding{owner, [](void* owner) noexcept -> std::span<Sample> {
                return static_cast<AudioBuffers*>(owner)->output;
            }};
        },
    };
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(graph.compiled_graph), GraphExecutorStageResult::accepted);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    PlaybackRenderSlice slice;
    while (buffers.early_calls.load(std::memory_order_acquire) != 2) {
        executor.render_block(4, 88200, &slice, true, 4);
        ASSERT_FALSE(slice.rendered);
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::yield();
    }
    EXPECT_EQ(executor.playback_snapshot().foreground_playhead, index(0));
    EXPECT_FLOAT_EQ(buffers.output.front().value, 0.f);

    (void)executor.set_playback_running(true);
    buffers.input.fill(Sample{.25f});
    do {
        executor.render_block(4, 88200, &slice, true, 4);
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        if (!slice.rendered) std::this_thread::yield();
    } while (!slice.rendered);
    EXPECT_EQ(slice.foreground_sample, index(0));
    EXPECT_FLOAT_EQ(buffers.output.front().value, 1.25f);
    EXPECT_FLOAT_EQ(buffers.output.back().value, 7.25f);

    auto const seek = executor.seek_playback(index(320));
    buffers.input.fill(Sample{.5f});
    buffers.output.fill({});
    executor.render_block(4, 88200, &slice);
    ASSERT_TRUE(slice.rendered);
    EXPECT_EQ(slice.foreground_sample, index(4));
    EXPECT_NE(executor.playback_snapshot().applied_command, seek);
    EXPECT_FLOAT_EQ(buffers.output.front().value, 9.5f);
    EXPECT_FLOAT_EQ(buffers.output.back().value, 15.5f);
    EXPECT_EQ(buffers.early_calls.load(), 2u);

    executor.render_block(4, 88200, &slice);
    ASSERT_TRUE(slice.rendered);
    EXPECT_EQ(slice.foreground_sample, index(320));
    EXPECT_EQ(executor.playback_snapshot().applied_command, seek);
}

TEST_F(GraphJitRuntimeFixture, PreparedTickPrefixMixesWithLiveInputAndDrainsBeforeSeek)
{
    auto graph = configured_module_graph(*revision, "iv.test.graph_jit.early_tick_audio_mix");
    auto compiled = compile_graph(graph, 703);
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    struct DeviceBuffers {
        std::array<Sample, 16> input{};
        std::array<Sample, 16> output{};
        std::thread::id foreground = std::this_thread::get_id();
        std::atomic<bool> called_from_worker{false};
        std::atomic<bool> hold_early_callback{false};
        std::atomic<unsigned> ramp_callbacks{0};

        bool on_foreground() noexcept
        {
            if (foreground == std::this_thread::get_id()) return true;
            called_from_worker.store(true, std::memory_order_relaxed);
            return false;
        }
    } device;
    resources.system_audio = {
        .owner = &device,
        .resolve_input_fn = [](void* owner, std::string_view id) {
            if (id == "early_tick_test_gate") {
                return SystemAudioInputBinding{owner, [](void* owner) noexcept -> std::span<Sample const> {
                    auto& selected = *static_cast<DeviceBuffers*>(owner);
                    selected.ramp_callbacks.fetch_add(1, std::memory_order_release);
                    selected.hold_early_callback.wait(true, std::memory_order_acquire);
                    return {};
                }};
            }
            return SystemAudioInputBinding{owner, [](void* owner) noexcept -> std::span<Sample const> {
                auto& selected = *static_cast<DeviceBuffers*>(owner);
                return selected.on_foreground() ? std::span<Sample const>{selected.input} : std::span<Sample const>{};
            }};
        },
        .resolve_output_fn = [](void* owner, std::string_view) {
            return SystemAudioOutputBinding{owner, [](void* owner) noexcept -> std::span<Sample> {
                auto& selected = *static_cast<DeviceBuffers*>(owner);
                return selected.on_foreground() ? std::span<Sample>{selected.output} : std::span<Sample>{};
            }};
        },
    };
    GraphExecutor executor(resources);
    struct ReleaseGate {
        std::atomic<bool>& held;
        void release() const
        {
            held.store(false, std::memory_order_release);
            held.notify_all();
        }
        ~ReleaseGate() { release(); }
    } gate{device.hold_early_callback};
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    auto wait_for_ramp = [&](unsigned calls) {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (device.ramp_callbacks.load(std::memory_order_acquire) < calls) {
            ASSERT_LT(std::chrono::steady_clock::now(), deadline);
            std::this_thread::yield();
        }
    };
    PlaybackRenderSlice slice;
    auto render_when_ready = [&](std::size_t frames, bool start_early_prefix = false) {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            executor.render_block(frames, 88200, &slice, start_early_prefix);
            if (slice.rendered) return;
            ASSERT_LT(std::chrono::steady_clock::now(), deadline);
            std::this_thread::yield();
        } while (true);
    };
    auto wait_for_paused_prefix = [&](std::uint64_t generation, GlobalIndex position) {
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            slice = {};
            executor.render_block(4, 88200, &slice, true);
            ASSERT_FALSE(slice.rendered);
            auto const playback = executor.playback_snapshot();
            if (executor.active_generation() == generation && !playback.foreground_running
                && playback.foreground_playhead == position && executor.playback_render_frames(64) == 4)
                return;
            ASSERT_LT(std::chrono::steady_clock::now(), deadline);
            std::this_thread::yield();
        } while (true);
    };
    device.hold_early_callback.store(true);
    ASSERT_NO_FATAL_FAILURE(wait_for_paused_prefix(703, index(0)));
    ASSERT_NO_FATAL_FAILURE(wait_for_ramp(1));

    // Queue the seek while the worker owns the old provisional reservation.
    auto const paused_seek = executor.seek_playback(index(20));
    executor.render_block(4, 88200, &slice, true);
    EXPECT_FALSE(slice.rendered);
    EXPECT_EQ(executor.playback_snapshot().foreground_playhead, index(0));
    EXPECT_NE(executor.playback_snapshot().applied_command, paused_seek);
    gate.release();
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        executor.render_block(4, 88200, &slice, true);
        ASSERT_FALSE(slice.rendered);
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::yield();
    } while (executor.playback_snapshot().applied_command != paused_seek);
    EXPECT_EQ(executor.playback_snapshot().foreground_playhead, index(20));
    EXPECT_EQ(executor.playback_snapshot().background_playhead, index(20));
    ASSERT_EQ(executor.playback_render_frames(64), 4u);

    (void)executor.set_playback_running(true);
    ASSERT_NO_FATAL_FAILURE(render_when_ready(4, true));
    EXPECT_FLOAT_EQ(device.output[0].value, 21.0f);
    EXPECT_FLOAT_EQ(device.output[6].value, 27.0f);
    auto const first = device.output;

    auto const callbacks = device.ramp_callbacks.load();
    device.hold_early_callback.store(true);
    executor.prepare_next_playback_slice(8, 88200);
    ASSERT_EQ(executor.playback_render_frames(64), 8u);
    ASSERT_NO_FATAL_FAILURE(wait_for_ramp(callbacks + 1));
    auto const blocked_seek = executor.seek_playback(index(300));
    device.input.fill(Sample{.25f});
    device.output.fill(Sample{});
    executor.render_block(3, 88200, &slice);
    EXPECT_FALSE(slice.rendered);
    EXPECT_EQ(executor.playback_snapshot().foreground_playhead, index(24));
    EXPECT_EQ(executor.playback_snapshot().background_playhead, index(24));
    EXPECT_NE(executor.playback_snapshot().applied_command, blocked_seek);
    gate.release();
    // The worker services reclamation after its current callback publishes.
    (void)executor.reclaim_retired_snapshots();
    auto const seek = executor.seek_playback(index(320));
    ASSERT_NO_FATAL_FAILURE(render_when_ready(3));
    EXPECT_EQ(slice.foreground_sample, index(24));
    EXPECT_FLOAT_EQ(device.output[0].value, 29.25f);
    EXPECT_FLOAT_EQ(device.output[4].value, 33.25f);
    EXPECT_NE(executor.playback_snapshot().applied_command, seek);
    auto const second = device.output;

    ASSERT_EQ(executor.playback_render_frames(64), 5u);
    device.input.fill(Sample{.5f});
    device.output.fill(Sample{});
    executor.render_block(5, 88200, &slice);
    ASSERT_TRUE(slice.rendered);
    EXPECT_FLOAT_EQ(device.output[0].value, 35.5f);
    EXPECT_FLOAT_EQ(device.output[8].value, 43.5f);
    EXPECT_NE(executor.playback_snapshot().applied_command, seek);
    auto const third = device.output;
    EXPECT_EQ(device.ramp_callbacks.load(), callbacks + 1);

    device.output.fill(Sample{});
    executor.render_block(2, 88200, &slice, true);
    ASSERT_TRUE(slice.rendered);
    EXPECT_EQ(slice.foreground_sample, index(320));
    EXPECT_EQ(executor.playback_snapshot().applied_command, seek);
    EXPECT_FLOAT_EQ(device.output[0].value, 333.5f);
    auto const after_seek = device.output;

    ProjectPlaybackRequest pause{.operation = ProjectPlaybackOperation::toggle_foreground};
    executor.handle_playback(pause);
    executor.render_block(4, 88200, &slice, true);
    ASSERT_FALSE(executor.playback_snapshot().foreground_running);
    auto reset = in_event_source([&] {
        return jit->handle_project_graph_transaction({
            .successor = {.project_generation = 704, .graph = graph, .definitions = definitions, .reset_state = true},
            .predecessor = compiled.compiled_graph,
        });
    });
    ASSERT_TRUE(reset.succeeded()) << (reset.diagnostics.empty() ? "" : reset.diagnostics.front().message);
    auto const callbacks_before_reset = device.ramp_callbacks.load();
    device.hold_early_callback.store(true);
    ASSERT_EQ(executor.stage(reset.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_NO_FATAL_FAILURE(wait_for_paused_prefix(704, index(320)));
    ASSERT_NO_FATAL_FAILURE(wait_for_ramp(callbacks_before_reset + 1));
    device.output.fill(Sample{});
    auto const play = executor.set_playback_running(true);
    executor.render_block(4, 88200, &slice, true);
    EXPECT_FALSE(slice.rendered);
    EXPECT_FALSE(executor.playback_snapshot().foreground_running);
    EXPECT_NE(executor.playback_snapshot().applied_command, play);
    gate.release();
    ASSERT_NO_FATAL_FAILURE(render_when_ready(4, true));
    EXPECT_EQ(executor.playback_snapshot().applied_command, play);
    EXPECT_FLOAT_EQ(device.output[0].value, 321.5f);
    EXPECT_FALSE(device.called_from_worker.load(std::memory_order_relaxed));

    // Compare the split execution and carried state against the complete root.
    GraphExecutor reference(resources);
    ASSERT_EQ(reference.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    (void)reference.seek_playback(index(20));
    (void)reference.set_playback_running(true);
    auto compare_complete_root = [&](std::size_t frames, float input, auto const& expected) {
        device.input.fill(Sample{input});
        device.output.fill(Sample{});
        PlaybackRenderSlice rendered;
        reference.render_block(frames, 88200, &rendered);
        ASSERT_TRUE(rendered.rendered);
        for (std::size_t i = 0; i < 2 * frames; ++i)
            EXPECT_FLOAT_EQ(device.output[i].value, expected[i].value);
    };
    ASSERT_NO_FATAL_FAILURE(compare_complete_root(4, 0.f, first));
    ASSERT_NO_FATAL_FAILURE(compare_complete_root(3, .25f, second));
    ASSERT_NO_FATAL_FAILURE(compare_complete_root(5, .5f, third));
    (void)reference.seek_playback(index(320));
    ASSERT_NO_FATAL_FAILURE(compare_complete_root(2, .5f, after_seek));

    auto next_reset = in_event_source([&] {
        return jit->handle_project_graph_transaction({
            .successor = {.project_generation = 705, .graph = graph, .definitions = definitions, .reset_state = true},
            .predecessor = reset.compiled_graph,
        });
    });
    ASSERT_TRUE(next_reset.succeeded()) << (next_reset.diagnostics.empty() ? "" : next_reset.diagnostics.front().message);
    executor.prepare_next_playback_slice(4, 88200);
    ASSERT_EQ(executor.playback_render_frames(64), 4u);
    ASSERT_NO_FATAL_FAILURE(render_when_ready(1));
    ASSERT_EQ(executor.playback_render_frames(64), 3u);
    ASSERT_EQ(executor.stage(next_reset.compiled_graph), GraphExecutorStageResult::accepted);
    EXPECT_FALSE(executor.activate_pending(index(325)));
    executor.render_block(3, 88200, &slice);
    ASSERT_TRUE(slice.rendered);
    EXPECT_EQ(slice.execution_generation, 704u);
    EXPECT_EQ(slice.foreground_sample, index(325));
    EXPECT_EQ(executor.playback_render_frames(64), 64u);
    auto const reset_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (executor.active_generation() != 705) {
        executor.render_block(1, 88200, &slice);
        ASSERT_LT(std::chrono::steady_clock::now(), reset_deadline);
        std::this_thread::yield();
    }
}

TEST_F(GraphJitRuntimeFixture, ResetRestoresFreshSequentialHistoryAndAlignsThePlayhead)
{
    for (auto const* module : {
            "iv.test.graph_jit.state_context.shared_neutral_compensated_module",
            "iv.test.graph_jit.state_context.detach_feedback_module",
        }) {
        SCOPED_TRACE(module);
        auto graph = configured_module_graph(*revision, module);
        ASSERT_TRUE(graph);
        auto const bundles = concrete_bundles(*graph);
        ASSERT_FALSE(bundles.empty());
        auto const& operation = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations.front();
        GraphJitCompileRequest request{
            .project_generation = 710,
            .graph = graph,
            .definitions = definitions,
            .observations = {.probes = {{.id = 1, .operation = &operation,
                .kind = ObservationSourceKind::input, .bundle = bundles.back(), .port = 0,
                .channels = {0}, .available = true}}},
        };
        auto initial = in_event_source([&] { return jit->handle_project_graph_transaction(
            {.successor = request, .initial_generation = true}); });
        ASSERT_TRUE(initial.succeeded()) << (initial.diagnostics.empty() ? "" : initial.diagnostics.front().message);
        request.project_generation = 711;
        request.reset_state = true;
        auto reset = in_event_source([&] { return jit->handle_project_graph_transaction(
            {.successor = request, .predecessor = initial.compiled_graph}); });
        ASSERT_TRUE(reset.succeeded()) << (reset.diagnostics.empty() ? "" : reset.diagnostics.front().message);
        if (auto const& cutover = reset.compiled_graph->cutover) EXPECT_TRUE(cutover->from_empty_graph);

        GraphExecutor executor(resources);
        ProjectPlaybackRequest resume{.operation = ProjectPlaybackOperation::toggle_foreground};
        executor.handle_playback(resume);
        auto render_first = [&](std::shared_ptr<CompiledGraph const> const& compiled, PlaybackRenderSlice& slice) {
            ASSERT_EQ(executor.stage(compiled), GraphExecutorStageResult::accepted);
            auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            do {
                executor.render_block(7, 48000, &slice);
                if (slice.execution_generation == compiled->project_generation) break;
                ASSERT_LT(std::chrono::steady_clock::now(), deadline);
                std::this_thread::yield();
            } while (true);
        };
        auto input_level = [&] {
            auto const* frame = executor.collect_observations();
            EXPECT_NE(frame, nullptr);
            EXPECT_TRUE(frame && frame->count == 1 && frame->values[0].value.captured);
            return frame ? frame->values[0].value.load<observation_details::SampleLevelState>()
                         : observation_details::SampleLevelState{};
        };

        PlaybackRenderSlice first;
        ASSERT_NO_FATAL_FAILURE(render_first(initial.compiled_graph, first));
        ASSERT_TRUE(first.rendered);
        auto const fresh_level = input_level();
        for (int block = 0; block < 5; ++block) executor.render_block(7, 48000);
        EXPECT_NE(input_level().peak, fresh_level.peak);
        EXPECT_NE(executor.playback_snapshot().foreground_playhead, index(0));

        PlaybackRenderSlice restarted;
        ASSERT_NO_FATAL_FAILURE(render_first(reset.compiled_graph, restarted));
        EXPECT_FALSE(restarted.rendered);
        EXPECT_EQ(restarted.foreground_sample, index(0));
        EXPECT_GT(restarted.segment, first.segment);
        executor.handle_playback(resume);
        executor.render_block(7, 48000, &restarted);
        ASSERT_TRUE(restarted.rendered);
        EXPECT_EQ(restarted.foreground_sample, index(0));
        auto const reset_level = input_level();
        EXPECT_EQ(reset_level.samples, 7u);
        EXPECT_DOUBLE_EQ(reset_level.peak, fresh_level.peak);
        EXPECT_DOUBLE_EQ(reset_level.sum_squares, fresh_level.sum_squares);
        EXPECT_EQ(executor.playback_snapshot().background_playhead, index(0));
        EXPECT_FALSE(executor.background_execution_failures().any());
    }
}

TEST_F(GraphJitRuntimeFixture, SummedOutputObservationReducesTheMixedSamplesOnce)
{
    auto member = configured_module_graph(*revision, "iv.test.graph_jit.state_context.transient_sample_module");
    auto const bundles = concrete_bundles(*member);
    ASSERT_GE(bundles.size(), 2u);
    GraphBuilder builder;
    auto const left = builder.embed(*member, "left").node_bundle(bundles.front());
    auto const right = builder.embed(*member, "right").node_bundle(bundles.front());
    auto graph = std::make_shared<ConfiguredGraph const>(builder.finish());
    auto const& operation = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations.front();
    auto compiled = jit->compile(GraphJitCompileRequest{
        .project_generation = 749, .graph = graph, .definitions = definitions,
        .observations = {.revision = 9, .probes = {{.id = 17, .operation = &operation,
            .kind = ObservationSourceKind::output, .channels = {0},
            .output_sources = {{{left, 0, 0}}, {{right, 0, 0}}}, .available = true}}},
    });
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(3)));
    executor.tick_block(index(3), 32);
    auto const* frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->count, 1u);
    auto const level = frame->values[0].value.load<observation_details::SampleLevelState>();
    EXPECT_EQ(level.samples, 32u);
    EXPECT_DOUBLE_EQ(level.peak, 68.0);
}

TEST_F(GraphJitRuntimeFixture, LiveInputObservationReadsTheEffectiveValueAfterOverride)
{
    enum class InputGesture { cpp_wire, project_gesture, cpp_gesture, settled_knob, settled_symbolic_knob };
    for (auto mode : {InputGesture::cpp_wire, InputGesture::project_gesture,
             InputGesture::cpp_gesture, InputGesture::settled_knob, InputGesture::settled_symbolic_knob}) {
        auto graph = std::make_shared<ConfiguredGraph>(
            *configured_module_graph(*revision, "iv.test.graph_jit.state_context.transient_sample_module"));
        auto const bundles = concrete_bundles(*graph);
        ASSERT_GE(bundles.size(), 2u);
        if (mode == InputGesture::project_gesture) {
            auto const wires = graph->connections.configured_sample_connections();
            std::vector samples(wires.begin(), wires.end());
            auto wire = std::ranges::find_if(samples, [&](auto const& connection) {
                return connection.target_channels.front().bundle == bundles[1];
            });
            ASSERT_NE(wire, samples.end());
            wire->source_owned = false;
            wire->target_symbolic_node.reset();
            graph->connections = GraphBuilderConnections::from_configured_connections(
                samples, graph->connections.configured_event_connections());
        }
        if (mode != InputGesture::cpp_wire) {
            bool const symbolic = mode == InputGesture::settled_symbolic_knob;
            bool const settled = mode == InputGesture::settled_knob || symbolic;
            graph->manual_controls.push_back({
                .id = {.node = {.instance_id = "sink"}, .port_name = "in"},
                .input = {bundles[1], PortKind::sample, 0},
                .settled_value = .25f,
                .override_connections = !settled, .enabled = settled,
            });
            if (symbolic) {
                auto const target = bundles[1];
                auto const symbol = graph->symbolic_nodes.create("sink", graph->node_bundles.bundle(target).type_identity());
                graph->symbolic_nodes.attach_bundle_member(graph->node_bundles, symbol, target);
                auto& control = graph->manual_controls.back();
                control.id.node.path.push_back(ProjectSymbolicNodeSelector{.source_identity = "sink"});
                control.input_symbolic_node = symbol;
            }
        }
        auto const& operation = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations.front();
        auto compiled = jit->compile(GraphJitCompileRequest{
            .project_generation = 740, .graph = graph, .definitions = definitions,
            .observations = {.revision = 31, .probes = {{.id = 41, .operation = &operation,
                .kind = ObservationSourceKind::input, .bundle = bundles[1], .port = 0,
                .channels = {0}, .available = true}}},
        });
        ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
        GraphExecutor executor(resources);
        ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
        ASSERT_TRUE(executor.activate_pending(index(3)));
        executor.tick_block(index(3), 32);
        auto const* frame = executor.collect_observations();
        ASSERT_NE(frame, nullptr);
        ASSERT_EQ(frame->count, 1u);
        ASSERT_TRUE(frame->values[0].value.captured);
        auto const level = frame->values[0].value.load<observation_details::SampleLevelState>();
        EXPECT_EQ(level.samples, 32u);
        auto const expected = mode == InputGesture::cpp_wire ? 34.0 : .25;
        EXPECT_DOUBLE_EQ(level.peak, expected);
    }
}

TEST_F(GraphJitRuntimeFixture, LiveInputObservationCanReplaceCodeWithoutChangingExecutionGeneration)
{
    auto graph = configured_module_graph(*revision, "iv.test.graph_jit.state_context.transient_sample_module");
    auto const bundles = concrete_bundles(*graph);
    ASSERT_GE(bundles.size(), 2u);
    auto const& operation = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations.front();
    GraphJitCompileRequest inputs{
        .project_generation = 761, .graph = graph, .definitions = definitions,
        .configured_bundles = {.snapshot = 1, .size = graph->node_bundles.size()},
    };
    auto predecessor = jit->compile(inputs);
    ASSERT_TRUE(predecessor.succeeded()) << (predecessor.diagnostics.empty() ? "" : predecessor.diagnostics.front().message);
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(predecessor.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    executor.tick_block(index(0), 16);

    inputs.project_generation = 762;
    inputs.observations = {.revision = 1, .probes = {{.id = 87, .operation = &operation,
        .kind = ObservationSourceKind::input, .bundle = bundles[1], .channels = {0}, .available = true}}};
    ProjectGraphRealtimeObservationRequest attachment{
        .execution_generation = 761, .update = inputs.observations,
    };
    EXPECT_FALSE(executor.stage_observation_update(attachment));

    auto replacement = in_event_source([&] {
        return jit->handle_project_graph_transaction({
            .successor = inputs, .predecessor = predecessor.compiled_graph,
            .active_observation_predecessor_code = predecessor.compiled_graph,
        });
    });
    ASSERT_TRUE(replacement.succeeded()) << (replacement.diagnostics.empty() ? "" : replacement.diagnostics.front().message);
    EXPECT_TRUE(replacement.reuses_observation_layout);
    EXPECT_FALSE(replacement.compiled_graph->cutover);
    ASSERT_EQ(replacement.timings.size(), 1u);
    EXPECT_FALSE(replacement.timings.front().transition);
    EXPECT_EQ(replacement.timings.front().lowering_transition_us, 0u);
    ASSERT_TRUE(executor.stage_observation_update({
        .execution_generation = 761, .update = replacement.compiled_graph,
    }));
    EXPECT_TRUE(executor.activate_pending(index(16)));
    EXPECT_EQ(executor.active_generation(), 761u);
    EXPECT_EQ(executor.active_graph(), predecessor.compiled_graph);
    executor.tick_block(index(16), 16);
    auto const* frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->count, 1u);
    EXPECT_EQ(frame->project_generation, 761u);
    EXPECT_EQ(frame->requirements_revision, 1u);
    auto const level = frame->values[0].value.load<observation_details::SampleLevelState>();
    EXPECT_EQ(level.samples, 16u);
    EXPECT_DOUBLE_EQ(level.peak, 31.0);

    auto& plan = std::get<CompiledObservationPlan>(attachment.update);
    plan.revision = 2;
    plan.probes.front().id = 88;
    ASSERT_TRUE(executor.stage_observation_update(attachment));
    EXPECT_FALSE(executor.activate_pending(index(32)));
    executor.tick_block(index(32), 16);
    frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->count, 1u);
    EXPECT_EQ(frame->requirements_revision, 2u);
    EXPECT_EQ(frame->values[0].request_id, 88u);
    EXPECT_DOUBLE_EQ(frame->values[0].value.load<observation_details::SampleLevelState>().peak, 47.0);
}

TEST_F(GraphJitRuntimeFixture, AddingAnOutputProbePreservesFeedbackAndSystemAudioThroughStorageCutover)
{
    auto feedback = configured_unequal_latency_projected_sample_feedback_graph(*revision);
    auto const bundles = concrete_bundles(*feedback);
    ASSERT_EQ(bundles.size(), 2u);
    auto loopback = configured_module_graph(*revision, "iv.test.graph_jit.system_audio_loopback");
    GraphBuilder builder;
    auto const observed_output = builder.embed(*feedback, "feedback").node_bundle(bundles[1]);
    builder.embed(*loopback, "audio");
    auto graph = std::make_shared<ConfiguredGraph const>(builder.finish());
    auto const& operation = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations.front();
    GraphJitCompileRequest inputs{
        .project_generation = 763, .graph = graph, .definitions = definitions,
        .configured_bundles = {.snapshot = 1, .size = graph->node_bundles.size()},
    };
    auto predecessor = jit->compile(inputs);
    ASSERT_TRUE(predecessor.succeeded()) << (predecessor.diagnostics.empty() ? "" : predecessor.diagnostics.front().message);
    inputs.observations = {.revision = 1, .probes = {{.id = 89, .operation = &operation,
        .kind = ObservationSourceKind::output, .bundle = observed_output, .channels = {0}, .available = true}}};
    inputs.project_generation = 764;
    auto cutover = in_event_source([&] {
        return jit->handle_project_graph_transaction({
            .successor = inputs, .predecessor = predecessor.compiled_graph,
            .active_observation_predecessor_code = predecessor.compiled_graph,
        });
    });
    ASSERT_TRUE(cutover.succeeded()) << (cutover.diagnostics.empty() ? "" : cutover.diagnostics.front().message);
    EXPECT_FALSE(cutover.reuses_observation_layout);
    ASSERT_TRUE(cutover.compiled_graph->cutover);
    ASSERT_EQ(cutover.timings.size(), 1u);
    EXPECT_TRUE(cutover.timings.front().transition);
    auto uninterrupted = jit->compile(inputs);
    ASSERT_TRUE(uninterrupted.succeeded()) << (uninterrupted.diagnostics.empty() ? "" : uninterrupted.diagnostics.front().message);

    struct DeviceBuffers {
        std::array<Sample, 128> capture{};
        std::array<Sample, 128> render{};
        std::size_t input_resolutions = 0;
        std::size_t output_resolutions = 0;
    } device;
    device.capture.fill(Sample{.25f});
    auto observed_resources = resources;
    observed_resources.system_audio = {
        .owner = &device,
        .resolve_input_fn = [](void* owner, std::string_view) {
            ++static_cast<DeviceBuffers*>(owner)->input_resolutions;
            return SystemAudioInputBinding{owner, [](void* owner) noexcept -> std::span<Sample const> {
                return static_cast<DeviceBuffers*>(owner)->capture;
            }};
        },
        .resolve_output_fn = [](void* owner, std::string_view) {
            ++static_cast<DeviceBuffers*>(owner)->output_resolutions;
            return SystemAudioOutputBinding{owner, [](void* owner) noexcept -> std::span<Sample> {
                return static_cast<DeviceBuffers*>(owner)->render;
            }};
        },
    };
    GraphExecutor reference(resources);
    GraphExecutor observed(observed_resources);
    ASSERT_EQ(reference.stage(uninterrupted.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_EQ(observed.stage(predecessor.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(reference.activate_pending(index(0)));
    ASSERT_TRUE(observed.activate_pending(index(0)));
    reference.tick_block(index(0), 64);
    observed.tick_block(index(0), 64);
    EXPECT_EQ(device.render, device.capture);
    EXPECT_EQ(device.input_resolutions, 1u);
    EXPECT_EQ(device.output_resolutions, 1u);
    ASSERT_FALSE(observed.stage_observation_update({
        .execution_generation = 763, .update = cutover.compiled_graph,
    }));
    ASSERT_EQ(observed.stage(cutover.compiled_graph), GraphExecutorStageResult::accepted);
    EXPECT_EQ(device.input_resolutions, 2u);
    EXPECT_EQ(device.output_resolutions, 2u);
    ASSERT_TRUE(observed.activate_pending(index(64)));
    EXPECT_EQ(observed.active_generation(), 764u);
    // One sample per frame makes a dropped in-flight sample visible in the
    // readout, even if subsequent samples would give the same block peak.
    for (auto sample = 64; sample < 80; ++sample) {
        SCOPED_TRACE(sample);
        device.render.fill(Sample{});
        reference.tick_block(index(sample), 1);
        observed.tick_block(index(sample), 1);
        auto const* expected = reference.collect_observations();
        auto const* actual = observed.collect_observations();
        ASSERT_NE(expected, nullptr);
        ASSERT_NE(actual, nullptr);
        auto const expected_level = expected->values[0].value.load<observation_details::SampleLevelState>();
        auto const actual_level = actual->values[0].value.load<observation_details::SampleLevelState>();
        EXPECT_EQ(expected_level.samples, 1u);
        EXPECT_EQ(actual_level.samples, 1u);
        EXPECT_DOUBLE_EQ(actual_level.peak, expected_level.peak);
        EXPECT_EQ(device.render[0].value, .25f);
        EXPECT_EQ(device.render[1].value, .25f);
    }
    EXPECT_EQ(device.input_resolutions, 2u);
    EXPECT_EQ(device.output_resolutions, 2u);
}

TEST_F(GraphJitRuntimeFixture, ObservationSinkWithBackgroundDeliveryImportsItsSequentialInitializer)
{
    auto graph = configured_module_graph(*revision,
        "iv.test.graph_jit.state_context.background_execution_module");
    auto const bundles = concrete_bundles(*graph);
    ASSERT_EQ(bundles.size(), 1u);
    auto const& operation = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations.front();
    // The compiler-level probe adds its ordinary Sequential sink. A Tock
    // source forces background delivery and generated neutral initialization
    // for the sink; a purely realtime source does not exercise that import.
    auto compiled = jit->compile(GraphJitCompileRequest{
        .project_generation = 733, .graph = graph, .definitions = definitions,
        .observations = {.revision = 29, .probes = {{.id = 71, .operation = &operation,
            .kind = ObservationSourceKind::output, .bundle = bundles.front(), .port = 0, .available = true}}},
    });
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    auto const& plan = compiled.compiled_graph->background_evaluation_plan;
    auto input = std::ranges::find_if(plan.ports, [](auto const& port) {
        return port.direction == graph_jit::PortDirection::input && port.name == "observed";
    });
    ASSERT_NE(input, plan.ports.end());
    ASSERT_TRUE(input->tick_sequential_input);
    auto initialize = compiled.compiled_graph->root_operations.initialize_sequential_input;
    ASSERT_NE(initialize, nullptr);
    std::array<Sample, 3> neutral{Sample{3}, Sample{4}, Sample{5}};
    EXPECT_TRUE(initialize(static_cast<std::size_t>(input - plan.ports.begin()),
        reinterpret_cast<std::byte*>(neutral.data()), sizeof(neutral)));
    for (auto value : neutral) EXPECT_FLOAT_EQ(value.value, 0);

    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool activated = false;
    while (!(activated = executor.activate_pending(index(0))) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_TRUE(activated);
    executor.tick_block(index(0), 32);
    auto const* frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->count, 1u);
    EXPECT_TRUE(frame->values[0].value.captured);
    auto const result = frame->values[0].value.load<observation_details::SampleLevelState>();
    EXPECT_EQ(result.samples, 32u);
    EXPECT_DOUBLE_EQ(result.peak, 12);
    EXPECT_FALSE(executor.background_execution_failures().any());
}

TEST_F(GraphJitRuntimeFixture, LiveStateObservationRunsAfterSpecializedCallbacksAndInvalidatesOnRemoval)
{
    auto graph = configured_module_graph(*revision, "iv.test.graph_jit.state_context.direct_sample_module");
    auto const bundles = concrete_bundles(*graph);
    ASSERT_GE(bundles.size(), 2u);
    // The consumer's reflected float State field is read through the same
    // registry codec as the generic frontend's numeric field operation.
    std::optional<NodeStateFieldStructure> field;
    std::size_t bundle_index = 0;
    std::string metadata;
    graph->node_bundles.for_each_configured_bundle([&](ConfiguredNodeBundleView const& bundle) {
        if (bundle_index++ != bundles[1]) return;
        metadata += bundle.operations ? "operations;" : "no operations;";
        if (!bundle.operations || !bundle.operations->runtime.state_structures) return;
        auto const& structure = bundle.operations->runtime.state_structures->state;
        metadata += structure ? "State;" : "no State;";
        if (structure) for (auto const& candidate : structure->fields) {
            metadata += candidate.name + ":" + candidate.type_name + ";";
            if (candidate.type_name == "c:f") { field = candidate; break; }
        }
    });
    ASSERT_TRUE(field) << "bundle " << bundles[1] << " State fields: " << metadata;
    auto compiled = jit->compile(GraphJitCompileRequest{
        .project_generation = 702, .graph = graph, .definitions = definitions,
        .observations = {.revision = 10, .probes = {{.id = 21,
            .operation = &observation_details::float_latest, .kind = ObservationSourceKind::state_field,
            .bundle = bundles[1], .state_field_offset = field->bit_offset / 8,
            .state_field_size = field->size_bits / 8, .available = true}}},
    });
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(3)));
    executor.tick_block(index(3), 16);
    auto const* collected = executor.collect_observations();
    ASSERT_NE(collected, nullptr);
    auto frame = *collected;
    EXPECT_TRUE(frame.values[0].value.captured);
    float observed;
    std::memcpy(&observed, frame.values[0].value.bytes.data(), sizeof(observed));
    EXPECT_FLOAT_EQ(observed, 3.0f);
    auto without = jit->handle_project_graph_transaction(ProjectGraphGraphJitRequest{
        .successor = {
            .project_generation = 703, .graph = std::make_shared<ConfiguredGraph const>(), .definitions = definitions,
            .observations = {.revision = 11, .probes = {{.id = 21}}},
        },
        .predecessor = compiled.compiled_graph,
    });
    ASSERT_TRUE(without.succeeded()) << (without.diagnostics.empty() ? "" : without.diagnostics.front().message);
    ASSERT_EQ(executor.stage(without.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(19)));
    executor.tick_block(index(19), 16);
    auto const* latest = executor.collect_observations();
    ASSERT_NE(latest, nullptr);
    EXPECT_GT(latest->activation_id, frame.activation_id);
    EXPECT_EQ(latest->values[0].status, ObservationValueStatus::unavailable);
}

TEST_F(GraphJitRuntimeFixture, LiveOutputObservationSelectsThePhysicalChannelInAnInterleavedOutput)
{
    auto graph = configured_module_graph(*revision, "iv.test.graph_jit.state_context.stereo_conversion_module");
    auto bundles = concrete_bundles(*graph);
    ASSERT_FALSE(bundles.empty());
    auto const& operation = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations.front();
    auto compiled = jit->compile(GraphJitCompileRequest{
        .project_generation = 704, .graph = graph, .definitions = definitions,
        .observations = {.revision = 12, .probes = {{.id = 22, .operation = &operation,
            .kind = ObservationSourceKind::output, .bundle = bundles.front(), .port = 0,
            .channels = {1}, .available = true}}},
    });
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(3)));
    executor.tick_block(index(3), 32);
    auto const* frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->values[0].value.load<observation_details::SampleLevelState>().samples, 32u);
    EXPECT_DOUBLE_EQ(frame->values[0].value.load<observation_details::SampleLevelState>().peak, 1034.0);
}

TEST_F(GraphJitRuntimeFixture, ModuleOutputMeterUsesItsResolvedSampleTypeAndPublicAddress)
{
    auto const module = definitions->by_id.find("iv.test.graph_jit.state_context.ported_module");
    ASSERT_NE(module, definitions->by_id.end());
    ASSERT_EQ(module->second.kind, NodeDefinitionKind::module);
    GraphExecutor executor(resources);
    NodeInstances instances;
    GraphConnections connections;
    // Keep bindings alive until ProjectGraph has joined its control worker.
    std::optional<project_graph_node_instances_bridge::scope> instances_scope;
    std::optional<project_graph_graph_connections_bridge::scope> connections_scope;
    std::optional<project_graph_graph_jit_bridge::scope> jit_scope;
    std::optional<project_graph_graph_executor_bridge::scope> executor_scope;
    ProjectGraph project;
    instances_scope.emplace(project, instances);
    connections_scope.emplace(project, connections);
    jit_scope.emplace(project, *jit);
    executor_scope.emplace(project, executor);
    NodeDefinitionsSnapshotChanged changed{.snapshot = definitions};
    in_event_source([&] { project.handle_node_definitions_snapshot_changed(changed); });
    ProjectStringBuilder create;
    in_event_source([&] { project.handle_project_create_node_instance({
        .instance_id = "module-meter", .definition_id = "iv.test.graph_jit.state_context.ported_module",
    }, create); });
    ASSERT_EQ(create.build(), "module-meter");
    ObservationGraphRequest describe{.describe_nodes = {{.instance_id = "module-meter"}}};
    in_event_source([&] { project.handle_observation_request(describe); });
    ASSERT_TRUE(describe.error.empty()) << describe.error;
    ASSERT_TRUE(describe.capabilities);
    ASSERT_TRUE(std::ranges::any_of(*describe.capabilities, [](auto const& capability) {
        return capability["kind"] == "output" && capability["member"] == "value";
    })) << describe.capabilities->dump();
    ObservationGraphRequest subscribe{.requirements = ObservationRequirements{.revision = 13,
        .probes = {{.id = 23, .source = {.node = {.instance_id = "module-meter"},
            .member = "value"}, .operation = "level"}}}};
    in_event_source([&] { project.handle_observation_request(subscribe); });
    ASSERT_TRUE(subscribe.error.empty()) << subscribe.error;
    ASSERT_TRUE(wait_for_project_compilation(project, executor, index(0)));
    ASSERT_TRUE(project.current_generation());
    ASSERT_TRUE(project.current_generation()->compiled_graph);
    for (std::int64_t block = 0; block < 4; ++block) {
        (void)executor.activate_pending(index(block * 64));
        ASSERT_TRUE(executor.active_generation());
        executor.tick_block(index(block * 64), 64);
    }
    auto const* frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->count, 1u);
    EXPECT_EQ(frame->requirements_revision, 13u);
    EXPECT_EQ(frame->values[0].value.load<observation_details::SampleLevelState>().samples, 64u);
    EXPECT_DOUBLE_EQ(frame->values[0].value.load<observation_details::SampleLevelState>().peak, .25);

    auto const execution = executor.active_generation();
    ASSERT_TRUE(execution);
    project.acknowledge_execution(*execution);
    auto const activation = frame->activation_id;
    ObservationGraphRequest unsubscribe{.requirements = ObservationRequirements{.revision = 14}};
    in_event_source([&] { project.handle_observation_request(unsubscribe); });
    ASSERT_TRUE(unsubscribe.error.empty()) << unsubscribe.error;
    ASSERT_TRUE(project.wait_for_compilation());
    EXPECT_EQ(project.current_generation()->execution_generation, *execution);
    EXPECT_FALSE(executor.activate_pending(index(256)));
    executor.tick_block(index(256), 64);
    frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->project_generation, *execution);
    EXPECT_GT(frame->activation_id, activation);
    EXPECT_EQ(frame->requirements_revision, 14u);
    EXPECT_EQ(frame->count, 0u);

    ProjectStringBuilder create_successor;
    in_event_source([&] { project.handle_project_create_node_instance({
        .instance_id = "another-module", .definition_id = "iv.test.graph_jit.state_context.ported_module",
    }, create_successor); });
    ASSERT_EQ(create_successor.build(), "another-module");
    ASSERT_TRUE(wait_for_project_compilation(project, executor, index(320)));
    EXPECT_EQ(project.current_generation()->execution_generation, *execution + 1);
}

TEST_F(GraphJitRuntimeFixture, StereoObservationKeepsIndependentLevelAndSignedValueResults)
{
    auto graph = configured_module_graph(*revision, "iv.test.graph_jit.state_context.stereo_conversion_module");
    auto const bundles = concrete_bundles(*graph);
    ASSERT_FALSE(bundles.empty());
    auto const operations = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations;
    auto const level = std::ranges::find(operations, "level", &ObservationOperation::id);
    auto const value = std::ranges::find(operations, "value", &ObservationOperation::id);
    ASSERT_NE(level, operations.end());
    ASSERT_NE(value, operations.end());
    auto compiled = jit->compile(GraphJitCompileRequest{
        .project_generation = 705, .graph = graph, .definitions = definitions,
        .observations = {.revision = 14, .probes = {
            {.id = 1, .operation = &*level, .bundle = bundles.front(), .port = 0, .channels = {0}, .available = true},
            {.id = 2, .operation = &*level, .bundle = bundles.front(), .port = 0, .channels = {1}, .available = true},
            {.id = 3, .operation = &*value, .bundle = bundles.front(), .port = 0, .channels = {0}, .available = true},
            {.id = 4, .operation = &*value, .bundle = bundles.front(), .port = 0, .channels = {1}, .available = true},
        }},
    });
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(-35)));
    executor.tick_block(index(-35), 32);
    auto const* frame = executor.collect_observations();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->count, 4u);
    auto const left = frame->values[0].value.load<observation_details::SampleLevelState>();
    auto const right = frame->values[1].value.load<observation_details::SampleLevelState>();
    EXPECT_EQ(left.samples, 32u);
    EXPECT_EQ(right.samples, 32u);
    EXPECT_DOUBLE_EQ(left.peak, 35);
    EXPECT_DOUBLE_EQ(right.peak, 996);
    EXPECT_FLOAT_EQ(frame->values[2].value.load<float>(), -4);
    EXPECT_FLOAT_EQ(frame->values[3].value.load<float>(), 996);
    EXPECT_FALSE(executor.background_execution_failures().any());
}

TEST_F(GraphJitRuntimeFixture, StereoObservationResolvesExplicitChannelSelectorsWithoutImplicitDownmix)
{
    auto graph = configured_module_graph(*revision, "iv.test.graph_jit.state_context.stereo_conversion_module");
    auto const bundles = concrete_bundles(*graph);
    ASSERT_FALSE(bundles.empty());
    std::unordered_map<std::string, NodeInstancePlacement> placements{
        {"stereo", {.root = bundles.front()}},
    };
    ObservationRequirements requests{.revision = 15, .probes = {
        {.id = 1, .source = {.node = {.instance_id = "stereo"}, .member = "out"}, .operation = "level"},
        {.id = 2, .source = {.node = {.instance_id = "stereo"}, .member = "out", .channel = 0}, .operation = "level"},
        {.id = 3, .source = {.node = {.instance_id = "stereo"}, .member = "out", .channel = 1}, .operation = "value"},
    }};
    auto resolved = resolve_observation_requirements(*graph, placements, requests);
    ASSERT_EQ(resolved.probes.size(), 3u);
    EXPECT_FALSE(resolved.probes[0].available);
    EXPECT_TRUE(resolved.probes[1].available);
    EXPECT_EQ(resolved.probes[1].channels, std::vector<std::size_t>{0});
    ASSERT_TRUE(resolved.probes[2].available);
    EXPECT_EQ(resolved.probes[2].channels, std::vector<std::size_t>{1});
    EXPECT_EQ(resolved.probes[2].operation->id, "value");
}
TEST_F(GraphJitRuntimeFixture, BackgroundInputWaveformsUseCoverageRegionsAndSequentialFallback)
{
    struct ExpectedRegion {
        GlobalIndexRange range;
        std::vector<float> values;
    };
    struct Case {
        std::string_view module, control, observed_input;
        std::int64_t begin;
        std::vector<ExpectedRegion> regions;
        bool override_connections = false;
        bool public_input = false;
        std::int64_t stride = 1;
        bool project_wire = false;
    };
    for (auto const& test : {
             Case{"iv.test.replay.coverage_manual_window", "in", "in", 18,
                 {{range(18, 20), {.25f, .25f}}, {range(20, 24), {4, 5, 6, 7}}, {range(24, 26), {.25f, .25f}}}, false, false, 1, true},
             Case{"iv.test.replay.coverage_manual_window", "in", "in", 18,
                 {{range(18, 26), std::vector<float>(8, .25f)}}},
             Case{"iv.test.replay.coverage_manual_window", "coverage", "in", 18,
                 {{range(18, 26), std::vector<float>(8, .25f)}}},
             // A project wire yields to the gesture. Only the requested endpoints of the manual Region are sampled.
             Case{"iv.test.replay.coverage_manual_window", "coverage", "in", 18,
                 {{range(18, 26), {.25f, .25f}}}, true, false, 7, true},
             Case{"iv.test.replay.coverage_manual_window", "coverage", "coverage", 18,
                 {{range(18, 20), {.25f, .25f}}, {range(20, 24), {4, 5, 6, 7}}, {range(24, 26), {.25f, .25f}}}, false, true, 1, true},
             Case{"iv.test.automation.transformed_sparse_curve", "gain", "gain", 0,
                 {{range(0, 8), {0, .5f, 1, 1.5f, .25f, .25f, .25f, .25f}}}, false, false, 1, true},
         }) {
        SCOPED_TRACE(::testing::Message{} << test.module << " control=" << test.control
            << " observed input=" << test.observed_input << " public=" << test.public_input
            << " stride=" << test.stride << " project wire=" << test.project_wire);
        auto configured = configured_graph_with_manual_input(test.module, test.control, .25f, test.override_connections);
        ASSERT_TRUE(configured);
        if (test.project_wire) {
            ASSERT_EQ(configured->manual_controls.size(), 1u);
            auto const input = configured->manual_controls.front().input;
            auto const wires = configured->connections.configured_sample_connections();
            std::vector samples(wires.begin(), wires.end());
            auto const wire = std::ranges::find_if(samples, [&](auto const& connection) {
                return std::ranges::any_of(connection.target_channels, [&](auto const& channel) {
                    return channel.bundle == input.node_bundle_handle && channel.port == input.port_index;
                });
            });
            ASSERT_NE(wire, samples.end());
            ASSERT_TRUE(wire->source_owned);
            auto cpp_wire = *wire;
            wire->source_owned = false;
            wire->target_symbolic_node.reset();
            samples.push_back(std::move(cpp_wire));
            configured->connections = GraphBuilderConnections::from_configured_connections(
                samples, configured->connections.configured_event_connections());
        }
        auto compiled = compile_graph(configured, 741);
        ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
        auto const& graph = *compiled.compiled_graph;
        auto const& plan = graph.background_evaluation_plan;
        NodeBundlePortId selected;
        if (test.public_input) {
            auto const port = std::ranges::find_if(plan.public_coverage_inputs, [&](auto const& input) {
                return graph.configured_graph->node_bundles.resolve_sample_input(input.port).config.name
                    == test.observed_input;
            });
            ASSERT_NE(port, plan.public_coverage_inputs.end());
            ASSERT_TRUE(port->reconstructible);
            selected = port->port;
        } else {
            auto const port = std::ranges::find_if(plan.ports, [&](auto const& port) {
                return port.direction == graph_jit::PortDirection::input && port.name == test.observed_input
                    && port.configured_port.node_bundle_handle < graph.configured_graph->node_bundles.size();
            });
            ASSERT_NE(port, plan.ports.end());
            selected = port->configured_port;
            ASSERT_TRUE(observation_input_window_port(graph, selected));
        }
        auto const operations = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations;
        auto const operation = std::ranges::find(operations, "waveform", &ObservationOperation::id);
        ASSERT_NE(operation, operations.end());
        auto query = std::make_shared<ObservationWindowQuery>();
        query->configured_generation = graph.configured_generation.value_or(graph.project_generation);
        query->kind = ObservationSourceKind::input;
        query->port = selected;
        query->operation = &*operation;
        constexpr std::int64_t window_length = 8;
        auto const points = static_cast<std::size_t>((window_length - 1) / test.stride + 1);
        query->window = {index(test.begin), window_length, points};

        GraphExecutor executor(resources);
        ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool activated = false;
        while (!(activated = executor.activate_pending(index(test.begin))) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_TRUE(activated);
        ObservationCollectRequest collection{.windows = {query}};
        executor.handle_observation_collect(collection);
        while (!query->complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_TRUE(query->complete.load(std::memory_order_acquire));
        ASSERT_EQ(query->result.status, ObservationWindowResult::Status::ready) << query->result.error;
        ASSERT_EQ(query->result.regions.size(), test.regions.size());
        for (auto const& expected : test.regions) {
            auto const observed = std::ranges::find_if(query->result.regions,
                [&](auto const& region) { return region.range == expected.range; });
            ASSERT_NE(observed, query->result.regions.end());
            ASSERT_EQ(observed->values.size(), expected.values.size());
            std::size_t value_index = 0;
            for (std::int64_t offset = 0; offset < window_length; offset += test.stride) {
                auto const requested = index(test.begin + offset);
                if (!expected.range.contains(requested)) continue;
                ASSERT_LT(value_index, expected.values.size());
                auto const& [position, value] = observed->values[value_index];
                EXPECT_EQ(position, requested);
                ASSERT_TRUE(value.captured);
                EXPECT_FLOAT_EQ(value.load<float>(), expected.values[value_index++]);
            }
            EXPECT_EQ(value_index, expected.values.size());
        }
    }
}

TEST_F(GraphJitRuntimeFixture, BeatGridObservationReadsRootAndNestedTimelineInputs)
{
    auto compiled = compile("iv.test.replay.project_beat_grid", 751);
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto const seam = index(std::numeric_limits<std::int64_t>::min());
    auto const initial = seam.retreated_by(44100);
    auto const panned = seam.retreated_by(22050);
    ObservationCollectRequest collection;
    for (auto const& input : graph.background_evaluation_plan.public_event_coverage_inputs) {
        if (input.type != EventTypeId::beat) continue;
        ASSERT_TRUE(input.reconstructible);
        ASSERT_EQ(input.sources.size(), 1u);
        for (auto const window : std::array{
            ObservationWindow{.begin = index(0), .length = 44101, .points = 32, .max_depth = 2},
            ObservationWindow{.begin = index(0), .length = 44101, .points = 32, .max_depth = 2, .include_indexed_runs = true},
            ObservationWindow{.begin = index(-3000000), .length = 6000001, .points = 4, .max_depth = 0},
            ObservationWindow{.begin = index(-3000000), .length = 6000001, .points = 4, .max_depth = 0, .include_indexed_runs = true},
            ObservationWindow{.begin = initial, .length = 88200, .points = 32, .max_depth = 2},
            ObservationWindow{.begin = initial, .length = 88200, .points = 32, .max_depth = 2, .include_indexed_runs = true},
            ObservationWindow{.begin = panned, .length = 88200, .points = 32, .max_depth = 2},
            ObservationWindow{.begin = panned, .length = 88200, .points = 32, .max_depth = 2, .include_indexed_runs = true},
        }) {
            auto& query = collection.windows.emplace_back(std::make_shared<ObservationWindowQuery>());
            query->configured_generation = graph.configured_generation.value_or(graph.project_generation);
            query->kind = ObservationSourceKind::input;
            query->port = input.port;
            query->window = window;
        }
    }
    ASSERT_EQ(collection.windows.size(), 16u);

    GraphExecutor executor(resources);
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    auto const activation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool activated = false;
    while (!(activated = executor.activate_pending(initial)) && std::chrono::steady_clock::now() < activation_deadline)
        std::this_thread::yield();
    ASSERT_TRUE(activated);

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    executor.handle_observation_collect(collection);
    using Mark = std::pair<std::int64_t, unsigned>;
    std::vector<std::vector<Mark>> observed;
    for (auto const& query : collection.windows) {
        while (!query->complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_TRUE(query->complete.load(std::memory_order_acquire));
        ASSERT_EQ(query->result.status, ObservationWindowResult::Status::ready) << query->result.error;
        ASSERT_TRUE(query->result.unavailable_regions.empty());
        auto& marks = observed.emplace_back();
        auto const window_order = [&](Mark const& mark) {
            return std::pair{query->window.begin.forward_distance_to(index(mark.first)), mark.second};
        };
        for (auto const& event : query->result.events)
            marks.emplace_back(event.time.signed_value(), std::get<BeatEvent>(event.value).depth);
        if (query->window.include_indexed_runs) {
            EXPECT_TRUE(query->result.events.empty());
            ASSERT_FALSE(query->result.beat_runs.empty());
            for (auto const& run : query->result.beat_runs) {
                EXPECT_EQ(run.indices.count, run.samples.size());
                EXPECT_TRUE(run.indices.fits(run.event_count));
                if (query->window.points == 4) EXPECT_GT(run.indices.step, 1u);
                for (auto const sample : run.samples) {
                    EXPECT_TRUE(run.region.contains(sample));
                    marks.emplace_back(sample.signed_value(), run.depth);
                }
            }
            std::ranges::sort(marks, {}, window_order);
        } else {
            EXPECT_TRUE(query->result.beat_runs.empty());
            EXPECT_TRUE(std::ranges::is_sorted(marks, {}, window_order));
        }
    }
    for (std::size_t query = 0; query < observed.size(); query += 2)
        EXPECT_EQ(observed[query], observed[query + 1]) << "Query " << query;
    for (auto const start : {0u, 8u}) {
        auto const& original = observed[start + 4];
        auto const& moved = observed[start + 6];
        ASSERT_FALSE(original.empty());
        EXPECT_GE(original.front().first, 0);
        EXPECT_LT(original.back().first, 0);
        auto in_window = [](GlobalIndex begin, Mark const& mark) {
            return begin.forward_distance_to(index(mark.first)) < 88200;
        };
        std::vector<Mark> before, after;
        std::ranges::copy_if(original, std::back_inserter(before), [&](auto const& mark) { return in_window(panned, mark); });
        std::ranges::copy_if(moved, std::back_inserter(after), [&](auto const& mark) { return in_window(initial, mark); });
        EXPECT_FALSE(before.empty());
        EXPECT_EQ(before, after);
    }
    std::vector fine{observed[0], observed[8]};
    std::ranges::sort(fine);
    EXPECT_EQ(fine, (std::vector<std::vector<Mark>>{
        {{0, 0}, {7350, 2}, {14700, 2}, {22050, 1}, {29400, 2}, {36750, 2}, {44100, 1}},
        {{0, 0}, {22050, 2}, {44100, 1}},
    }));
}

TEST_F(GraphJitRuntimeFixture, PublicCoverageObservationReadsBeforeInternalDownmixAndAllowsDisconnection)
{
    auto original = configured_module_graph(*revision, "iv.test.replay.stereo_coverage_boundary");
    auto const bundles = original->node_bundles.size();
    NodeBundleHandle boundary = 0;
    while (boundary < bundles && !original->node_bundles.bundle(boundary).is_subgraph()) ++boundary;
    ASSERT_LT(boundary, bundles);
    for (bool connected : {true, false}) {
        SCOPED_TRACE(connected);
        auto configured = std::make_shared<ConfiguredGraph>(*original);
        if (!connected) {
            auto const old = configured->connections.configured_sample_connections();
            std::vector<ConfiguredSampleConnection> connections;
            for (auto const& connection : old) {
                if (connection.target_channels.front().bundle != boundary) connections.push_back(connection);
            }
            configured->connections = GraphBuilderConnections::from_configured_connections(
                connections, configured->connections.configured_event_connections());
        }
        auto compiled = compile_graph(configured, 744);
        ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
        auto const& graph = *compiled.compiled_graph;
        auto const& plan = graph.background_evaluation_plan;
        ASSERT_EQ(plan.public_coverage_inputs.size(), 1u);
        auto const& input = plan.public_coverage_inputs.front();
        EXPECT_TRUE(input.reconstructible);
        EXPECT_EQ(input.projections.empty(), !connected);
        EXPECT_EQ(plan.ports.end(), std::ranges::find(plan.ports, input.port, &graph_jit::BackgroundPortPlan::configured_port));

        std::unordered_map<std::string, NodeInstancePlacement> placements{{"module", {.root = boundary}}};
        auto query = std::make_shared<ObservationWindowQuery>();
        ASSERT_TRUE(resolve_observation_window_source(*configured, placements,
            {.node = {.instance_id = "module"}, .kind = ObservationSourceKind::input, .member = "coverage", .channel = 1},
            "waveform", *query, &graph.port_realization, &graph));
        query->configured_generation = graph.configured_generation.value_or(graph.project_generation);
        query->window = {index(18), 8, 8};
        GraphExecutor executor(resources);
        ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!executor.activate_pending(index(18)) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ObservationCollectRequest collection{.windows = {query}};
        executor.handle_observation_collect(collection);
        while (!query->complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_TRUE(query->complete.load(std::memory_order_acquire));
        ASSERT_EQ(query->result.status, ObservationWindowResult::Status::ready) << query->result.error;
        if (!connected) {
            EXPECT_TRUE(query->result.regions.empty());
            continue;
        }
        ASSERT_EQ(query->result.regions.size(), 1u);
        auto const& region = query->result.regions.front();
        EXPECT_EQ(region.range, range(20, 24));
        ASSERT_EQ(region.values.size(), 4u);
        for (std::size_t i = 0; i < region.values.size(); ++i) {
            EXPECT_EQ(region.values[i].first, index(20 + static_cast<std::int64_t>(i)));
            ASSERT_TRUE(region.values[i].second.captured);
            EXPECT_FLOAT_EQ(region.values[i].second.load<float>(), 104 + static_cast<float>(i));
        }
    }
}

TEST_F(GraphJitRuntimeFixture, PublicCoverageObservationDoesNotRequireReplayOfAnUnusedSource)
{
    auto compiled = compile("iv.test.replay.unused_coverage_input", 745);
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    auto const& plan = compiled.compiled_graph->background_evaluation_plan;
    ASSERT_EQ(plan.public_coverage_inputs.size(), 1u);
    EXPECT_FALSE(plan.public_coverage_inputs.front().reconstructible);
    EXPECT_TRUE(plan.ports.empty());
    EXPECT_TRUE(plan.runtime.bindings.empty());
}

TEST_F(GraphJitRuntimeFixture, BackgroundWaveformQueriesProjectSparseComputedOutputsBeforeStorageExpires)
{
    auto compiled = compile("iv.test.automation.transformed_sparse_curve", 731);
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto const& plan = graph.background_evaluation_plan;
    for (std::size_t node = 0; node < plan.nodes.size(); ++node) {
        if (!plan.nodes[node].authored_tock_execution) continue;
        auto const& invocation = plan.tick_runtime.nodes[node];
        EXPECT_EQ(invocation.random_access_sample_count, 0u);
        EXPECT_EQ(invocation.random_access_event_count, 0u);
    }
    auto port = std::ranges::find_if(plan.ports, [](auto const& port) {
        return port.kind == PortKind::sample && port.authored_tock_output && port.name == "value";
    });
    ASSERT_NE(port, plan.ports.end());
    auto const& operations = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations;
    auto operation = std::ranges::find(operations, "waveform", &ObservationOperation::id);
    ASSERT_NE(operation, operations.end());
    auto query = std::make_shared<ObservationWindowQuery>();
    query->configured_generation = graph.configured_generation.value_or(graph.project_generation);
    query->port = port->configured_port;
    query->operation = &*operation;
    query->window = {index(0), 8, 8};
    {
        GraphExecutor executor(resources);
        ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool activated = false;
        while (!(activated = executor.activate_pending(index(0))) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_TRUE(activated);
        auto submit = [&](std::shared_ptr<ObservationWindowQuery> const& request) {
            ObservationCollectRequest collection{.windows = {request}};
            executor.handle_observation_collect(collection);
            while (!request->complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            return request->complete.load(std::memory_order_acquire);
        };
        ASSERT_TRUE(submit(query));
        ASSERT_EQ(query->result.status, ObservationWindowResult::Status::ready) << query->result.error;
        ASSERT_EQ(query->result.regions.size(), 1u);
        EXPECT_EQ(query->result.regions[0].range, range(0, 4));
        ASSERT_EQ(query->result.regions[0].values.size(), 4u);
        EXPECT_FLOAT_EQ(query->result.regions[0].values[0].second.load<float>(), 0);
        EXPECT_FLOAT_EQ(query->result.regions[0].values[3].second.load<float>(), 1.5f);
        ObservationCollectRequest after_first;
        executor.handle_observation_collect(after_first);

        auto distant = std::make_shared<ObservationWindowQuery>();
        distant->configured_generation = query->configured_generation;
        distant->port = query->port;
        distant->operation = query->operation;
        distant->window = {index(0), std::uint64_t{1} << 40, 512};
        ASSERT_TRUE(submit(distant));
        ASSERT_EQ(distant->result.status, ObservationWindowResult::Status::ready) << distant->result.error;
        ASSERT_EQ(distant->result.regions.size(), 1u);
        ASSERT_EQ(distant->result.regions[0].values.size(), 1u);
        EXPECT_EQ(distant->result.regions[0].values[0].first, index(0));
        ObservationCollectRequest after;
        executor.handle_observation_collect(after);
        EXPECT_EQ(after.execution_data_version, after_first.execution_data_version);
        EXPECT_FALSE(executor.background_execution_failures().any());
    }
    // No transaction view or generation-local storage backs the returned value.
    EXPECT_FLOAT_EQ(query->result.regions[0].values[3].second.load<float>(), 1.5f);
}

TEST_F(GraphJitRuntimeFixture, SparseComputedWaveformFitsBelowTheBufferedMappingRunLimit)
{
    auto compiled = compile("iv.test.computed.huge_playback", 746);
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto const& plan = graph.background_evaluation_plan;
    auto const port = std::ranges::find(plan.ports, std::string{"huge"}, &graph_jit::BackgroundPortPlan::name);
    ASSERT_NE(port, plan.ports.end());
    ASSERT_TRUE(port->authored_tock_output);
    GraphExecutor executor(resources, {}, BufferedStorageLimits{.max_mapping_runs = 64});
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool activated = false;
    while (!(activated = executor.activate_pending(index(0))) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_TRUE(activated);

    // These samples are on separate pages, well outside the initial contiguous
    // playback window. Caching each observed sample would run out of mappings.
    auto const begin = index(1 << 20);
    constexpr std::size_t points = 512, stride = 8193;
    std::shared_ptr<ObservationWindowQuery> query;
    do {
        query = std::make_shared<ObservationWindowQuery>();
        query->configured_generation = graph.configured_generation.value_or(graph.project_generation);
        query->port = port->configured_port;
        query->operation = &observation_details::sample_operations.back();
        query->window = {begin, 1 + (points - 1) * stride, points};
        ObservationCollectRequest collection{.windows = {query}};
        executor.handle_observation_collect(collection);
        while (!query->complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_TRUE(query->complete.load(std::memory_order_acquire));
    } while (query->result.status == ObservationWindowResult::Status::pending_execution
        && std::chrono::steady_clock::now() < deadline);
    ASSERT_EQ(query->result.status, ObservationWindowResult::Status::ready)
        << query->result.error;
    ASSERT_EQ(query->result.regions.size(), 1u);
    auto const& region = query->result.regions.front();
    EXPECT_EQ(region.range, (GlobalIndexRange{begin, query->window.length}));
    ASSERT_EQ(region.values.size(), points);
    for (std::size_t i = 0; i < points; ++i) {
        auto const& [position, captured] = region.values[i];
        EXPECT_EQ(position, begin.advanced_by(i * stride));
        ASSERT_TRUE(captured.captured);
        EXPECT_FLOAT_EQ(captured.load<float>(), static_cast<float>(position.bits() & 1023));
    }
    EXPECT_FALSE(executor.background_execution_failures().any());
}

TEST_F(GraphJitRuntimeFixture, BackgroundWaveformReadsPublishedTickRecordingWithoutReplayingTheProducer)
{
    auto compiled = compile("iv.test.graph_jit.state_context.tick_capture_module", 732);
    ASSERT_TRUE(compiled.succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    auto graph_owner = std::const_pointer_cast<CompiledGraph>(compiled.compiled_graph);
    auto& graph = *graph_owner;
    auto port = std::ranges::find_if(graph.background_evaluation_plan.ports, [](auto const& port) {
        return port.kind == PortKind::sample && port.persisted_tick_output;
    });
    ASSERT_NE(port, graph.background_evaluation_plan.ports.end());

    struct EvaluationGate {
        CompiledGraphBackgroundFunction evaluate;
        std::atomic<bool> entered{false};
        std::atomic<bool> held{true};
        void release()
        {
            held.store(false, std::memory_order_release);
            held.notify_all();
        }
    } gate{graph.background_operations.evaluate};
    static EvaluationGate* current_gate;
    current_gate = &gate;
    graph.background_operations.evaluate = [](graph_jit::BackgroundEvaluationCall* call) {
        auto& gate = *current_gate;
        if (!gate.entered.exchange(true, std::memory_order_acq_rel))
            gate.held.wait(true, std::memory_order_acquire);
        gate.evaluate(call);
    };
    GraphExecutor executor(resources);
    struct ReleaseGate {
        EvaluationGate& gate;
        ~ReleaseGate() { gate.release(); }
    } release{gate};
    ASSERT_EQ(executor.stage(compiled.compiled_graph), GraphExecutorStageResult::accepted);
    ASSERT_TRUE(executor.activate_pending(index(0)));
    executor.tick_block(index(0), 32);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!gate.entered.load(std::memory_order_acquire)) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::yield();
    }
    // Publish another recording while the worker is handling the first one.
    // The visible waveform request needs the second recording's snapshot.
    executor.tick_block(index(32), 32);
    auto query = std::make_shared<ObservationWindowQuery>();
    query->configured_generation = graph.configured_generation.value_or(graph.project_generation);
    query->port = port->configured_port;
    query->operation = &observation_details::sample_operations.back();
    query->window = {index(36), 4, 4};
    ObservationCollectRequest collection{.windows = {query}};
    executor.handle_observation_collect(collection);
    gate.release();
    while (!query->complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_TRUE(query->complete.load(std::memory_order_acquire));
    ASSERT_EQ(query->result.status, ObservationWindowResult::Status::ready) << query->result.error;
    ASSERT_EQ(query->result.regions.size(), 1u);
    EXPECT_EQ(query->result.regions[0].range, range(36, 40));
    ASSERT_EQ(query->result.regions[0].values.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(query->result.regions[0].values[i].second.captured);
        EXPECT_FLOAT_EQ(query->result.regions[0].values[i].second.load<float>(), static_cast<float>(i) + 36.5f);
    }
    EXPECT_GT(query->result.data_version, 0u);
    EXPECT_FALSE(executor.background_execution_failures().any());
}

TEST_F(GraphJitRuntimeFixture, ClickDetectorRetainsPlaybackHistoryAcrossMeterChangesAndSeek)
{
    GraphExecutor executor(resources);
    NodeInstances instances;
    GraphConnections connections;
    // Keep bindings alive until ProjectGraph has joined its control worker.
    std::optional<project_graph_node_instances_bridge::scope> instances_scope;
    std::optional<project_graph_graph_connections_bridge::scope> connections_scope;
    std::optional<project_graph_graph_jit_bridge::scope> jit_scope;
    std::optional<project_graph_graph_executor_bridge::scope> executor_scope;
    ProjectGraph project;
    instances_scope.emplace(project, instances);
    connections_scope.emplace(project, connections);
    jit_scope.emplace(project, *jit);
    executor_scope.emplace(project, executor);
    NodeDefinitionsSnapshotChanged changed{.snapshot = definitions};
    in_event_source([&] { project.handle_node_definitions_snapshot_changed(changed); });
    ProjectAckBuilder load;
    in_event_source([&] { project.handle_project_replace_graph({
        .instances = {
            {.instance_id = "source", .definition_id = "iv.test.click_recording.source"},
            {.instance_id = "second-source", .definition_id = "iv.test.click_recording.source"},
            {.instance_id = "third-source", .definition_id = "iv.test.click_recording.source"},
            {.instance_id = "detector", .definition_id = "builtin.click_detector"},
        },
        .connections = {ProjectSampleConnection{
            .connection_id = "inspect-audio",
            .source_type = ChannelTypeId::stereo,
            .outputs = {
                {.instance_id = "source", .port = {.name = "audio"}},
                {.instance_id = "second-source", .port = {.name = "audio"}},
            },
            .target_type = ChannelTypeId::stereo,
            .inputs = {{.instance_id = "detector", .port = {.name = "audio"}}},
        }, ProjectSampleConnection{
            .connection_id = "inspect-third-source",
            .source_type = ChannelTypeId::stereo,
            .outputs = {{.instance_id = "third-source", .port = {.name = "audio"}}},
            .target_type = ChannelTypeId::stereo,
            .inputs = {{.instance_id = "detector", .port = {.name = "audio"}}},
        }},
        // Each source steps by 0.25. It takes both cables and all three
        // sources to cross the threshold on the first sample of the click.
        .manual_controls = {{
            .id = {.node = {.instance_id = "detector"}, .port_name = "threshold"},
            .stored_value = Sample{0.625f},
        }},
    }, load); });
    ASSERT_NO_THROW(load.build());

    auto activate = [&](std::size_t at) {
        auto const target = project.current_generation();
        if (!target || !target->graph_executor_accepted) return false;
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            (void)executor.activate_pending(index(at));
            if (target->execution_generation
                && executor.active_generation() == target->execution_generation
                && executor.background_generation() == target->execution_generation) {
                project.acknowledge_execution(*target->execution_generation);
                return true;
            }
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    ASSERT_TRUE(wait_for_project_compilation(project, executor, index(0)));
    ASSERT_TRUE(activate(0));
    (void)executor.set_playback_running(true);

    auto const graph = executor.active_graph();
    ASSERT_TRUE(graph);
    auto const sample_rate = graph->specialization.sample_rate;
    auto const half_second = sample_rate / 2;
    auto const recording_frames = 5 * half_second;
    auto const block_size = graph->specialization.block_size;
    std::size_t rendered = 0;
    auto render_until = [&](std::size_t end) {
        while (rendered < end) {
            auto const frames = std::min(block_size, end - rendered);
            executor.render_block(frames, sample_rate);
            rendered += frames;
        }
        EXPECT_EQ(executor.playback_snapshot().foreground_playhead, index(rendered));
    };
    auto data_version = [&] {
        ObservationCollectRequest collection;
        executor.handle_observation_collect(collection);
        return collection.execution_data_version;
    };
    ObservationSource const source{
        .node = {.instance_id = "detector"}, .kind = ObservationSourceKind::output,
        .member = "clicks", .port_kind = PortKind::event,
    };
    auto read_recording = [&](std::size_t points = 0) {
        ObservationGraphRequest observation{.windows = {{
            source, points ? recorded_trigger_bins_operation : recorded_event_timestamps_operation,
            {index(0), recording_frames, points},
        }}};
        in_event_source([&] { project.handle_observation_request(observation); });
        EXPECT_TRUE(observation.error.empty()) << observation.error;
        auto query = observation.windows.front().query;
        if (!query) return query;
        ObservationCollectRequest collection{.windows = {query}};
        executor.handle_observation_collect(collection);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!query->complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return query;
    };

    std::vector<GlobalIndex> expected;
    for (std::size_t i = 0; i < 5; ++i) {
        auto const click = i * half_second + 13;
        auto const previous_version = data_version();
        render_until((click / block_size + 1) * block_size);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (data_version() == previous_version && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_GT(data_version(), previous_version) << "Click at sample " << click << " was never published";

        expected.push_back(index(click));
        auto const query = read_recording();
        ASSERT_TRUE(query);
        ASSERT_TRUE(query->complete.load(std::memory_order_acquire));
        ASSERT_EQ(query->result.status, ObservationWindowResult::Status::ready) << query->result.error;
        EXPECT_EQ(query->result.event_indices, expected) << "After recording sample " << click;

        if (i == 1) {
            // Entering a viewport with a live meter can activate a new
            // execution of the same authored project graph.
            ObservationGraphRequest subscribe{.requirements = ObservationRequirements{.revision = 1,
                .probes = {{.id = 17, .source = {.node = {.instance_id = "source"},
                    .member = "audio", .channel = 0}, .operation = "level"}}}};
            in_event_source([&] { project.handle_observation_request(subscribe); });
            ASSERT_TRUE(subscribe.error.empty()) << subscribe.error;
            ASSERT_TRUE(project.wait_for_compilation());
            ASSERT_TRUE(activate(rendered));
            auto const metered = executor.active_graph();
            ASSERT_TRUE(metered);
            EXPECT_NE(metered->project_generation, graph->project_generation);
            EXPECT_EQ(metered->configured_generation, graph->configured_generation);
            ASSERT_EQ(metered->observations.probes.size(), 1u);
            EXPECT_TRUE(metered->observations.probes.front().available);
            auto const after_cutover = read_recording();
            ASSERT_TRUE(after_cutover);
            ASSERT_TRUE(after_cutover->complete.load(std::memory_order_acquire));
            ASSERT_EQ(after_cutover->result.status, ObservationWindowResult::Status::ready)
                << after_cutover->result.error;
            EXPECT_EQ(after_cutover->result.event_indices, expected);
        }
    }

    render_until(recording_frames);
    // A running seek replays a click at a different offset within the
    // callback, so the new write overlaps the original recording.
    auto const before_replay = data_version();
    (void)executor.seek_playback(index(half_second + 5));
    executor.render_block(block_size, sample_rate);
    ASSERT_EQ(executor.playback_snapshot().foreground_playhead, index(half_second + 5 + block_size));
    auto const replay_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (data_version() == before_replay && std::chrono::steady_clock::now() < replay_deadline)
        std::this_thread::yield();
    ASSERT_GT(data_version(), before_replay);
    auto const final = read_recording();
    ASSERT_TRUE(final);
    ASSERT_TRUE(final->complete.load(std::memory_order_acquire));
    ASSERT_EQ(final->result.status, ObservationWindowResult::Status::ready) << final->result.error;
    EXPECT_EQ(final->result.event_indices, expected);
    auto const overview = read_recording(2);
    ASSERT_TRUE(overview);
    ASSERT_TRUE(overview->complete.load(std::memory_order_acquire));
    ASSERT_EQ(overview->result.status, ObservationWindowResult::Status::ready) << overview->result.error;
    ASSERT_EQ(overview->result.trigger_bins.size(), 2u);
    EXPECT_EQ(overview->result.trigger_bins[0].first, expected[0]);
    EXPECT_EQ(overview->result.trigger_bins[0].count, 3u);
    EXPECT_EQ(overview->result.trigger_bins[1].first, expected[3]);
    EXPECT_EQ(overview->result.trigger_bins[1].count, 2u);
    EXPECT_FALSE(executor.background_execution_failures().any());
}

} // namespace iv::test::graph_jit_tests
