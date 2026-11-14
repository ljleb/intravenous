#include <intravenous/runtime/audio_input_synchronizer.h>
#include <intravenous/runtime/graph_executor.h>
#include <intravenous/runtime/system_audio_devices.h>
#include "../event_source_test_support.h"
#include <intravenous/runtime/project_graph.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/project_graph_system_audio_devices_bridge.h>
#include <intravenous/runtime/project_graph_package_jit_bridge.h>
#include <intravenous/runtime/project_graph_node_instances_bridge.h>
#include <intravenous/runtime/project_graph_graph_connections_bridge.h>
#include <intravenous/runtime/package_jit.h>
#include <intravenous/runtime/project_persistence_builder.h>
#include <intravenous/runtime/socket_rpc_requests.h>
#include <intravenous/runtime/socket_rpc_response_builders.h>
#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace {
struct FakeOutputRenderState {
    std::vector<iv::Sample> buffer{};
    std::mutex mutex{};
    std::condition_variable cv{};
    std::size_t requested_samples = 0;
    bool requested = false;
    bool submitted = false;
    bool shutdown = false;
};

struct DeviceLifetimeState {
    std::atomic<size_t> output_shutdowns = 0;
    std::atomic<size_t> input_shutdowns = 0;
    std::mutex output_mutex{};
    std::shared_ptr<FakeOutputRenderState> output{};
};

class FakeOutputDevice {
    iv::RenderConfig config_;
    std::shared_ptr<DeviceLifetimeState> lifetime_;
    std::shared_ptr<FakeOutputRenderState> render_;

public:
    FakeOutputDevice(iv::RenderConfig config, std::shared_ptr<DeviceLifetimeState> lifetime)
        : config_(config)
        , lifetime_(std::move(lifetime))
        , render_(std::make_shared<FakeOutputRenderState>())
    {
        render_->buffer.resize(config.max_block_frames * config.num_channels);
        std::scoped_lock lock(lifetime_->output_mutex);
        lifetime_->output = render_;
    }

    FakeOutputDevice(FakeOutputDevice&& other) noexcept
        : config_(other.config_)
        , lifetime_(std::move(other.lifetime_))
        , render_(std::move(other.render_))
    {}

    iv::RenderConfig const& config() const { return config_; }
    std::span<iv::Sample> wait_for_block_request()
    {
        std::unique_lock lock(render_->mutex);
        render_->cv.wait(lock, [&] {
            return render_->requested || render_->shutdown;
        });
        render_->requested = false;
        return render_->shutdown
            ? std::span<iv::Sample>{}
            : std::span{render_->buffer}.first(render_->requested_samples);
    }
    void submit_response()
    {
        {
            std::scoped_lock lock(render_->mutex);
            render_->submitted = true;
        }
        render_->cv.notify_all();
    }
    void request_shutdown()
    {
        {
            std::scoped_lock lock(render_->mutex);
            if (render_->shutdown) return;
            render_->shutdown = true;
            ++lifetime_->output_shutdowns;
        }
        render_->cv.notify_all();
    }
};

class FakeInputDevice {
    iv::RenderConfig config_;
    std::shared_ptr<DeviceLifetimeState> lifetime_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool shutdown_ = false;

public:
    FakeInputDevice(iv::RenderConfig config, std::shared_ptr<DeviceLifetimeState> lifetime)
        : config_(config)
        , lifetime_(std::move(lifetime))
    {}

    FakeInputDevice(FakeInputDevice&& other) noexcept
        : config_(other.config_)
        , lifetime_(std::move(other.lifetime_))
    {}

    iv::RenderConfig const& config() const { return config_; }

    iv::AudioInputBlock wait_for_captured_block()
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return shutdown_; });
        throw std::logic_error("fake input stopped");
    }

    void release_captured_block() {}

    void request_shutdown()
    {
        {
            std::scoped_lock lock(mutex_);
            if (shutdown_) return;
            shutdown_ = true;
            ++lifetime_->input_shutdowns;
        }
        cv_.notify_all();
    }
};

iv::SystemAudioDevicesBackend make_backend(std::shared_ptr<DeviceLifetimeState> lifetime)
{
    return {
        .list_output_devices = [] {
            return std::vector<iv::AudioDeviceDescriptor>{
                {.device_id = "default", .name = "System Default"},
                {.device_id = "out-1", .name = "Output 1"},
            };
        },
        .list_input_devices = [] {
            return std::vector<iv::AudioDeviceDescriptor>{
                {.device_id = "default", .name = "System Default"},
                {.device_id = "in-1", .name = "Input 1"},
            };
        },
        .make_output_device = [lifetime](std::string const&, iv::RenderConfig const& config) {
            return iv::AudioOutputDevice(
                std::in_place_type<FakeOutputDevice>, config, lifetime);
        },
        .make_input_device = [lifetime](std::string const&, iv::RenderConfig const& config) {
            return iv::AudioInputDevice(
                std::in_place_type<FakeInputDevice>, config, lifetime);
        },
    };
}

bool combine_sample_values(
    iv::graph_jit::BackgroundPortIndex,
    std::byte*,
    std::byte const*,
    std::size_t)
{
    return true;
}

iv::SystemAudioOutputBinding selected_output_binding{};
iv::SystemAudioOutputBinding missing_output_binding{};
std::atomic<std::size_t> render_probe_calls = 0;

void render_binding_probe(
    iv::graph_jit::RealtimeRootPlan const*,
    std::byte*,
    iv::graph_jit::TickInvocationCall const*,
    iv::GlobalIndex,
    std::size_t block_size)
{
    render_probe_calls.fetch_add(1, std::memory_order_relaxed);
    std::array<iv::Sample, 128> selected{};
    std::array<iv::Sample, 128> mixed{};
    std::array<iv::Sample, 128> missing{};
    auto const sample_count = block_size * 2;
    std::ranges::fill(selected, iv::Sample{0.25f});
    std::ranges::fill(mixed, iv::Sample{0.5f});
    std::ranges::fill(missing, iv::Sample{10.0f});
    selected_output_binding.write(std::span{selected}.first(sample_count));
    missing_output_binding.write(std::span{missing}.first(sample_count));
    selected_output_binding.write(std::span{mixed}.first(sample_count));
}

std::shared_ptr<FakeOutputRenderState> current_output(
    std::shared_ptr<DeviceLifetimeState> const& lifetime)
{
    std::scoped_lock lock(lifetime->output_mutex);
    return lifetime->output;
}

std::vector<iv::Sample> request_output_block(
    std::shared_ptr<FakeOutputRenderState> const& output,
    std::size_t frames)
{
    std::unique_lock lock(output->mutex);
    output->requested_samples = frames * 2;
    output->submitted = false;
    output->requested = true;
    output->cv.notify_all();
    (void)output->cv.wait_for(lock, std::chrono::seconds(5), [&] {
        return output->submitted || output->shutdown;
    });
    if (!output->submitted) return {};
    return {
        output->buffer.begin(),
        output->buffer.begin()
            + static_cast<std::ptrdiff_t>(output->requested_samples)};
}

TEST(AudioInputSynchronizer, PullsRateTowardTargetBufferDepth)
{
    iv::AudioInputSynchronizer too_full(48000, 64, 48000, 128);
    std::vector<iv::Sample> captured(512 * 2, iv::Sample{0.25f});
    too_full.push_captured_block({
        .samples = captured,
        .capture_timestamp_seconds = 0.0,
        .discontinuous = false,
    });
    EXPECT_GT(too_full.render_timeline_block().frame_count, 0u);
    EXPECT_GT(too_full.last_rate_ratio(), 1.0);

    iv::AudioInputSynchronizer too_empty(48000, 64, 48000, 256);
    std::vector<iv::Sample> sparse(64 * 2, iv::Sample{0.5f});
    too_empty.push_captured_block({
        .samples = sparse,
        .capture_timestamp_seconds = 0.0,
        .discontinuous = false,
    });
    EXPECT_EQ(too_empty.render_timeline_block().frame_count, 64u);
    EXPECT_LT(too_empty.last_rate_ratio(), 1.0);
}

TEST(SystemAudioDevices, OwnsOnlySelectionAndDeviceLifetime)
{
    auto lifetime = std::make_shared<DeviceLifetimeState>();
    iv::SystemAudioDevices devices(48000, 64, make_backend(lifetime));

    auto initial = devices.audio_devices_snapshot();
    ASSERT_EQ(initial.output_devices.size(), 2u);
    ASSERT_EQ(initial.input_devices.size(), 2u);
    EXPECT_EQ(initial.selected_output.device_id, std::optional<std::string>{"default"});
    EXPECT_EQ(initial.selected_input.device_id, std::optional<std::string>{"default"});
    EXPECT_TRUE(initial.selected_output.available);
    EXPECT_TRUE(initial.selected_input.available);

    auto changed = devices.set_selected_devices("out-1", "in-1");
    EXPECT_EQ(changed.selected_output.device_id, std::optional<std::string>{"out-1"});
    EXPECT_EQ(changed.selected_input.device_id, std::optional<std::string>{"in-1"});
    EXPECT_GE(lifetime->output_shutdowns.load(), 1u);
    EXPECT_GE(lifetime->input_shutdowns.load(), 1u);
}

TEST(SystemAudioDevices, MissingSelectionIsPreservedButUnavailable)
{
    auto lifetime = std::make_shared<DeviceLifetimeState>();
    iv::SystemAudioDevices devices(48000, 64, make_backend(lifetime));

    auto snapshot = devices.set_selected_devices("missing-output", "missing-input");
    EXPECT_EQ(snapshot.selected_output.device_id, std::optional<std::string>{"missing-output"});
    EXPECT_EQ(snapshot.selected_input.device_id, std::optional<std::string>{"missing-input"});
    EXPECT_FALSE(snapshot.selected_output.available);
    EXPECT_FALSE(snapshot.selected_input.available);

    auto resources = devices.resources();
    auto input = resources.system_audio.resolve_input("missing-input");
    auto output = resources.system_audio.resolve_output("missing-output");
    std::array<iv::Sample, 8> samples{};
    std::ranges::fill(samples, iv::Sample{1.0f});
    input.read(samples);
    for (auto const sample : samples) {
        EXPECT_FLOAT_EQ(sample.value, 0.0f);
    }
    EXPECT_NO_THROW(output.write(samples));
}

TEST(SystemAudioDevices, MissingOutputUsesFallbackGraphClock)
{
    auto lifetime = std::make_shared<DeviceLifetimeState>();
    iv::SystemAudioDevices devices(
        48000,
        64,
        make_backend(lifetime),
        "missing-output",
        "missing-input");
    selected_output_binding = {};
    missing_output_binding = {};
    iv::GraphExecutor executor(devices.resources());
    auto graph = std::make_shared<iv::CompiledGraph>();
    graph->project_generation = 1;
    graph->specialization = {
        .sample_rate = 48000,
        .block_size = 64,
    };
    graph->node_layout = iv::NodeLayoutBuilder(64).build();
    graph->root_operations = {
        .tick_block = {.function = &render_binding_probe},
        .combine_sequential_input = &combine_sample_values,
    };
    ASSERT_EQ(
        executor.stage(graph),
        iv::GraphExecutorStageResult::accepted);
    render_probe_calls.store(0, std::memory_order_relaxed);
    devices.attach_graph_executor(executor);

    auto const deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds{2};
    while (render_probe_calls.load(std::memory_order_relaxed) == 0
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    devices.detach_graph_executor(executor);
    devices.stop();
    EXPECT_GT(render_probe_calls.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(executor.active_generation(), 1u);
    EXPECT_FALSE(devices.audio_devices_snapshot().selected_output.available);
}

TEST(SystemAudioDevices, OutputClockActivatesAndRendersTheGraphExecutor)
{
    auto lifetime = std::make_shared<DeviceLifetimeState>();
    iv::SystemAudioDevices devices(48000, 64, make_backend(lifetime));
    auto resources = devices.resources();
    selected_output_binding = resources.system_audio.resolve_output("default");
    missing_output_binding = resources.system_audio.resolve_output("missing-output");

    iv::GraphExecutor executor(resources);
    auto graph = std::make_shared<iv::CompiledGraph>();
    graph->project_generation = 1;
    graph->specialization = {
        .sample_rate = 48000,
        .block_size = 64,
    };
    graph->node_layout = iv::NodeLayoutBuilder(64).build();
    graph->root_operations = {
        .tick_block = {.function = &render_binding_probe},
        .combine_sequential_input = &combine_sample_values,
    };
    ASSERT_EQ(
        executor.stage(graph),
        iv::GraphExecutorStageResult::accepted);
    devices.attach_graph_executor(executor);

    auto output = current_output(lifetime);
    ASSERT_TRUE(output);
    auto const rendered = request_output_block(output, 64);

    devices.detach_graph_executor(executor);
    devices.stop();
    ASSERT_EQ(rendered.size(), 128u);
    EXPECT_EQ(executor.active_generation(), 1u);
    for (auto const sample : rendered) {
        EXPECT_FLOAT_EQ(sample.value, 0.75f);
    }
}

TEST(SystemAudioDevices, OutputClockSlicesLargeDeviceRequestsIntoProjectBlocks)
{
    auto lifetime = std::make_shared<DeviceLifetimeState>();
    iv::AudioOutputDiagnostics diagnostics;
    iv::SystemAudioDevices devices(48000, 64, make_backend(lifetime),
        "default", "default", &diagnostics);
    auto resources = devices.resources();
    selected_output_binding = resources.system_audio.resolve_output("default");
    missing_output_binding = resources.system_audio.resolve_output("missing-output");

    iv::GraphExecutor executor(resources);
    auto graph = std::make_shared<iv::CompiledGraph>();
    graph->project_generation = 1;
    graph->specialization = {
        .sample_rate = 48000,
        .block_size = 64,
    };
    graph->node_layout = iv::NodeLayoutBuilder(64).build();
    graph->root_operations = {
        .tick_block = {.function = &render_binding_probe},
        .combine_sequential_input = &combine_sample_values,
    };
    ASSERT_EQ(
        executor.stage(graph),
        iv::GraphExecutorStageResult::accepted);
    render_probe_calls.store(0, std::memory_order_relaxed);
    devices.attach_graph_executor(executor);

    auto output = current_output(lifetime);
    ASSERT_TRUE(output);
    auto const rendered = request_output_block(output, 160);

    devices.detach_graph_executor(executor);
    devices.stop();
    ASSERT_EQ(rendered.size(), 320u);
    EXPECT_EQ(render_probe_calls.load(std::memory_order_relaxed), 3u);
    for (auto const sample : rendered) {
        EXPECT_FLOAT_EQ(sample.value, 0.75f);
    }
    auto const probe = diagnostics.rendered.read();
    ASSERT_TRUE(probe.has_value());
    EXPECT_EQ(probe->generation, 1u);
    EXPECT_EQ(probe->tick_calls, 3u);
    EXPECT_EQ(probe->binding_calls, 9u);
    EXPECT_EQ(probe->matched_bindings, 6u);
    EXPECT_EQ(probe->samples, 320u);
    EXPECT_DOUBLE_EQ(probe->peak, .75);
    EXPECT_DOUBLE_EQ(probe->sum, .75 * 320);
    EXPECT_DOUBLE_EQ(probe->square_sum, .75 * .75 * 320);
    EXPECT_EQ(probe->nonfinite, 0u);
}

TEST(SystemAudioDevices, AudioProbeDistinguishesMissingGraphFromSilentActiveOutput)
{
    auto lifetime = std::make_shared<DeviceLifetimeState>();
    iv::AudioOutputDiagnostics diagnostics;
    iv::SystemAudioDevices devices(48000, 64, make_backend(lifetime),
        "default", "default", &diagnostics);
    auto const output = current_output(lifetime);
    ASSERT_TRUE(output);
    auto const rendered = request_output_block(output, 64);
    devices.stop();
    ASSERT_EQ(rendered.size(), 128u);
    auto const probe = diagnostics.rendered.read();
    ASSERT_TRUE(probe);
    EXPECT_EQ(probe->generation, 0u);
    EXPECT_EQ(probe->tick_calls, 0u);
    EXPECT_EQ(probe->binding_calls, 0u);
    EXPECT_EQ(probe->matched_bindings, 0u);
    EXPECT_EQ(probe->samples, rendered.size());
    EXPECT_DOUBLE_EQ(probe->peak, 0);
    EXPECT_EQ(probe->nonfinite, 0u);
    EXPECT_FALSE(diagnostics.playback.read()); // Fake backend has no native callback.
}

TEST(AudioOutputDiagnostics, SeparatesFiniteSignalDcAndInvalidValues)
{
    iv::AudioOutputProbe probe;
    std::array<iv::Sample, 6> values{1.f, -1.f, .5f, 0.f,
        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
    probe.measure(values);
    iv::AudioOutputProbeSlot slot;
    EXPECT_FALSE(slot.read());
    slot.publish(probe);
    auto const observation = slot.read();
    ASSERT_TRUE(observation);
    EXPECT_EQ(observation->samples, 6u);
    EXPECT_EQ(observation->nonfinite, 2u);
    EXPECT_DOUBLE_EQ(observation->peak, 1);
    EXPECT_DOUBLE_EQ(observation->sum, .5);
    EXPECT_DOUBLE_EQ(observation->square_sum, 2.25);
}

TEST(AudioOutputDiagnostics, SamplesOneBlockPerSecondWithoutScanningEveryBlock)
{
    iv::AudioOutputProbeRate rate;
    EXPECT_TRUE(rate.due(64, 48000));
    for (std::size_t block = 0; block < 749; ++block) EXPECT_FALSE(rate.due(64, 48000));
    EXPECT_TRUE(rate.due(64, 48000));
    EXPECT_EQ(rate.blocks(), 751u);
}

TEST(SystemAudioDevices, LogicalOutputBindingFollowsDeviceSelection)
{
    auto lifetime = std::make_shared<DeviceLifetimeState>();
    iv::SystemAudioDevices devices(48000, 64, make_backend(lifetime));
    auto resources = devices.resources();
    selected_output_binding = resources.system_audio.resolve_output("out-1");
    missing_output_binding = resources.system_audio.resolve_output("missing-output");

    iv::GraphExecutor executor(resources);
    auto graph = std::make_shared<iv::CompiledGraph>();
    graph->project_generation = 1;
    graph->specialization = {
        .sample_rate = 48000,
        .block_size = 64,
    };
    graph->node_layout = iv::NodeLayoutBuilder(64).build();
    graph->root_operations = {
        .tick_block = {.function = &render_binding_probe},
        .combine_sequential_input = &combine_sample_values,
    };
    ASSERT_EQ(
        executor.stage(graph),
        iv::GraphExecutorStageResult::accepted);
    devices.attach_graph_executor(executor);

    auto default_output = current_output(lifetime);
    ASSERT_TRUE(default_output);
    auto const before_selection = request_output_block(default_output, 64);
    ASSERT_EQ(before_selection.size(), 128u);
    EXPECT_TRUE(std::ranges::all_of(before_selection, [](iv::Sample sample) {
        return sample.value == 0.0f;
    }));

    auto const selection = devices.set_selected_devices("out-1", "default");
    EXPECT_TRUE(selection.selected_output.available);
    auto selected_output = current_output(lifetime);
    ASSERT_TRUE(selected_output);
    EXPECT_NE(selected_output, default_output);
    auto const after_selection = request_output_block(selected_output, 64);

    devices.detach_graph_executor(executor);
    devices.stop();
    ASSERT_EQ(after_selection.size(), 128u);
    for (auto const sample : after_selection) {
        EXPECT_FLOAT_EQ(sample.value, 0.75f);
    }
}
} // namespace

namespace {
struct SelectionBackendState {
    bool fail_output = false;
    bool fail_input = false;
    bool hide_default = false;
    bool fail_enumeration = false;
    std::size_t output_creates = 0;
    std::size_t input_creates = 0;
};

iv::SystemAudioDevicesBackend history_backend(std::shared_ptr<DeviceLifetimeState> lifetime,
    std::shared_ptr<SelectionBackendState> state)
{
    auto backend = make_backend(lifetime);
    auto outputs = backend.list_output_devices;
    auto inputs = backend.list_input_devices;
    backend.list_output_devices = [state, outputs] {
        if (state->fail_enumeration) throw std::runtime_error("output enumeration failed");
        auto result = outputs();
        if (state->hide_default) std::erase_if(result, [](auto const& value) { return value.device_id == "default"; });
        return result;
    };
    backend.list_input_devices = [state, inputs] {
        if (state->fail_enumeration) throw std::runtime_error("input enumeration failed");
        auto result = inputs();
        if (state->hide_default) std::erase_if(result, [](auto const& value) { return value.device_id == "default"; });
        return result;
    };
    auto make_output = backend.make_output_device;
    auto make_input = backend.make_input_device;
    backend.make_output_device = [state, make_output](std::string const& id, iv::RenderConfig const& config) {
        ++state->output_creates;
        if (state->fail_output && id == "out-1") throw std::runtime_error("output open failed");
        return make_output(id, config);
    };
    backend.make_input_device = [state, make_input](std::string const& id, iv::RenderConfig const& config) {
        ++state->input_creates;
        if (state->fail_input && id == "in-1") throw std::runtime_error("input open failed");
        return make_input(id, config);
    };
    return backend;
}

struct AudioSettingsHistory : ::testing::Test {
    std::shared_ptr<DeviceLifetimeState> lifetime = std::make_shared<DeviceLifetimeState>();
    std::shared_ptr<SelectionBackendState> backend = std::make_shared<SelectionBackendState>();
    iv::ProjectGraph graph;
    iv::SystemAudioDevices devices{48000, 64, history_backend(lifetime, backend)};
    iv::PackageJit jit{iv::StartupConfigState{}};
    iv::project_graph_system_audio_devices_bridge::scope audio_scope{graph, devices};
    iv::project_graph_package_jit_bridge::scope toolchain_scope{graph, jit};

    iv::ProjectGraphPublication apply(iv::ProjectEditBatch const& edit) {
        iv::ProjectAckBuilder result;
        in_event_source([&] { graph.handle_project_apply_edit(edit, result); });
        result.build();
        return result.take_publication();
    }
    void undo(std::function<void()> const& authorize = {}) {
        iv::ProjectAckBuilder result;
        in_event_source([&] { graph.handle_project_undo(result, authorize); });
        result.build();
    }
    void redo() {
        iv::ProjectAckBuilder result;
        in_event_source([&] { graph.handle_project_redo(result); });
        result.build();
    }
    iv::ProjectEditBatch selection(std::optional<std::string> output = "out-1",
        std::optional<std::string> input = "in-1") {
        iv::ProjectEditBatch edit;
        edit.audio_devices = iv::ProjectAudioDeviceSelection{std::move(output), std::move(input)};
        return edit;
    }
};
} // namespace

TEST_F(AudioSettingsHistory, SelectionAndUndoRedoNeedNoGraphServicesOrCompilation)
{
    auto const original = devices.selected_devices();
    auto edit = selection();
    auto publication = apply(edit);
    EXPECT_FALSE(publication.graph_changed);
    EXPECT_FALSE(graph.current_generation());
    EXPECT_EQ(devices.selected_devices(), *edit.audio_devices);
    undo();
    EXPECT_EQ(devices.selected_devices(), original);
    redo();
    EXPECT_EQ(devices.selected_devices(), *edit.audio_devices);
    EXPECT_FALSE(graph.current_generation());
    // No NodeInstances, GraphConnections, GraphJit or GraphExecutor bridge is
    // installed. Any attempt to rebuild would fail this test.
}

TEST_F(AudioSettingsHistory, UndoRestoresAnUnpluggedSelectionAndReportsAvailabilitySeparately)
{
    apply(selection());
    backend->hide_default = true;
    EXPECT_NO_THROW(undo());
    auto status = devices.audio_devices_snapshot();
    EXPECT_EQ(status.selected_output.device_id, std::optional<std::string>{"default"});
    EXPECT_EQ(status.selected_input.device_id, std::optional<std::string>{"default"});
    EXPECT_FALSE(status.selected_output.available);
    EXPECT_FALSE(status.selected_input.available);
    EXPECT_TRUE(status.selected_output.error);
    EXPECT_TRUE(status.selected_input.error);
    EXPECT_NO_THROW(redo());
    status = devices.audio_devices_snapshot();
    EXPECT_TRUE(status.selected_output.available);
    EXPECT_TRUE(status.selected_input.available);
    EXPECT_FALSE(status.selected_output.error);
    EXPECT_FALSE(status.selected_input.error);
}

TEST_F(AudioSettingsHistory, HardwareFailureAcceptsBothIdsAndRemainsOneUndoStep)
{
    backend->fail_output = true;
    iv::SocketRpcAudioDevicesResultBuilder response;
    in_event_source([&] { graph.handle_socket_rpc_set_audio_devices({"out-1", "in-1"}, response); });
    auto json = nlohmann::ordered_json::parse(response.build(1));
    ASSERT_FALSE(json.contains("error"));
    EXPECT_EQ(json["result"]["selectedOutput"]["deviceId"], "out-1");
    EXPECT_EQ(json["result"]["selectedOutput"]["error"], "output open failed");
    EXPECT_EQ(json["result"]["selectedOutput"]["available"], false);
    EXPECT_EQ(json["result"]["selectedInput"]["deviceId"], "in-1");
    EXPECT_EQ(json["result"]["selectedInput"]["available"], true);
    undo();
    EXPECT_EQ(devices.selected_devices(), (iv::ProjectAudioDeviceSelection{"default", "default"}));
    redo();
    EXPECT_EQ(devices.selected_devices(), (iv::ProjectAudioDeviceSelection{"out-1", "in-1"}));
    EXPECT_TRUE(devices.audio_devices_snapshot().selected_input.available);
}

TEST_F(AudioSettingsHistory, InputActivationFailureDoesNotRejectOrDisableTheSelectedOutput)
{
    backend->fail_input = true;
    apply(selection());
    auto status = devices.audio_devices_snapshot();
    EXPECT_TRUE(status.selected_output.available);
    EXPECT_FALSE(status.selected_output.error);
    EXPECT_FALSE(status.selected_input.available);
    EXPECT_EQ(status.selected_input.error, std::optional<std::string>{"input open failed"});
    undo();
    EXPECT_TRUE(devices.audio_devices_snapshot().selected_input.available);
}

TEST_F(AudioSettingsHistory, NoopSelectionDoesNotRestartHardwareOrDiscardRedo)
{
    apply(selection());
    undo();
    auto const outputs = backend->output_creates;
    auto const inputs = backend->input_creates;
    apply(selection("default", "default"));
    EXPECT_EQ(backend->output_creates, outputs);
    EXPECT_EQ(backend->input_creates, inputs);
    EXPECT_NO_THROW(redo());
    EXPECT_EQ(devices.selected_devices(), (iv::ProjectAudioDeviceSelection{"out-1", "in-1"}));
}

TEST_F(AudioSettingsHistory, AudioUndoRequiresPermissionAndChecksTheActualOwnerRecord)
{
    apply(selection());
    EXPECT_THROW(undo([] { throw std::runtime_error("audio.control denied"); }), std::runtime_error);
    EXPECT_EQ(devices.selected_devices(), (iv::ProjectAudioDeviceSelection{"out-1", "in-1"}));
    (void)devices.set_selected_devices("out-1", std::nullopt);
    EXPECT_THROW(undo(), std::runtime_error);
    EXPECT_EQ(devices.selected_devices(), (iv::ProjectAudioDeviceSelection{"out-1", std::nullopt}));
    (void)devices.set_selected_devices("out-1", "in-1");
    EXPECT_NO_THROW(undo()); // Neither rejected undo advanced history.
    EXPECT_EQ(devices.selected_devices(), (iv::ProjectAudioDeviceSelection{"default", "default"}));
}

TEST_F(AudioSettingsHistory, SavesAcceptedSelectionEvenWhenEnumerationFails)
{
    apply(selection());
    backend->fail_enumeration = true;
    iv::ProjectPersistenceBuilder builder(std::filesystem::temp_directory_path(), {});
    EXPECT_NO_THROW(devices.handle_project_persistence_collect_state(builder));
    auto commands = builder.build();
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].command, "project.overrideSettings");
    EXPECT_EQ(commands[0].args["output_device_id"], "out-1");
    EXPECT_EQ(commands[0].args["input_device_id"], "in-1");
    EXPECT_NO_THROW(undo());
    EXPECT_EQ(devices.selected_devices(), (iv::ProjectAudioDeviceSelection{"default", "default"}));
    EXPECT_TRUE(devices.audio_devices_snapshot().selected_output.error);
}

TEST_F(AudioSettingsHistory, ToolchainSettingsShareHistoryButDoNotChangeRuntimeFlagsOrBuild)
{
    auto config = jit.toolchain_config();
    config.source_introspection = false;
    config.clang_time_trace = true;
    jit.set_toolchain_config(config);
    iv::ProjectEditBatch edit;
    edit.toolchain = iv::ProjectToolchainSettings{.cxx_compiler = "/missing/compiler",
        .iv_package_pch = "/missing/pch"};
    apply(edit);
    EXPECT_EQ(jit.toolchain_config().cxx_compiler, std::optional<std::filesystem::path>{"/missing/compiler"});
    EXPECT_FALSE(jit.toolchain_config().source_introspection);
    EXPECT_TRUE(jit.toolchain_config().clang_time_trace);
    EXPECT_FALSE(graph.current_generation());
    apply(selection());
    undo(); // Audio selection is the latest global action.
    EXPECT_EQ(jit.toolchain_config().cxx_compiler, edit.toolchain->cxx_compiler);
    undo();
    EXPECT_EQ(jit.toolchain_config().cxx_compiler, config.cxx_compiler);
    EXPECT_EQ(jit.toolchain_config().iv_package_pch, config.iv_package_pch);
    redo();
    EXPECT_EQ(jit.toolchain_config().iv_package_pch, edit.toolchain->iv_package_pch);
    EXPECT_FALSE(jit.toolchain_config().source_introspection);
    EXPECT_TRUE(jit.toolchain_config().clang_time_trace);
    EXPECT_FALSE(graph.current_generation());
    iv::PackageJitBatchRequest build{.declarations = {{.package_id = "requested-build", .package_root = "/missing/package"}}};
    EXPECT_NO_THROW(jit.handle_build_request(build));
    ASSERT_EQ(build.result.failed.size(), 1u);
    EXPECT_TRUE(build.result.revisions.empty());
    EXPECT_EQ(build.result.failed[0].package_id, "requested-build");
    EXPECT_EQ(jit.toolchain_config().iv_package_pch, edit.toolchain->iv_package_pch);
}

TEST_F(AudioSettingsHistory, MixedGraphAndSettingsBatchValidatesAllRecordsBeforeCommit)
{
    iv::NodeInstances instances;
    iv::GraphConnections connections;
    iv::project_graph_node_instances_bridge::scope nodes_scope{graph, instances};
    iv::project_graph_graph_connections_bridge::scope connections_scope{graph, connections};
    auto const original = devices.selected_devices();
    auto edit = selection();
    edit.toolchain = iv::ProjectToolchainSettings{.cxx_compiler = "/other/compiler"};
    edit.instances = {{"pending", iv::ProjectNodeInstanceDeclaration{
        .instance_id = "pending", .definition_id = "pending.definition", .package_root = "/missing/package"}}};
    edit.connections = {{"bad", iv::ProjectSampleConnection{.connection_id = "bad"}}};
    EXPECT_THROW(apply(edit), std::invalid_argument);
    EXPECT_TRUE(instances.desired_instances().empty());
    EXPECT_EQ(devices.selected_devices(), original);
    EXPECT_FALSE(jit.toolchain_config().cxx_compiler);
    edit.connections.clear();
    EXPECT_NO_THROW(apply(edit)); // The unavailable definition is accepted intent.
    EXPECT_EQ(instances.desired_instances().size(), 1u);
    EXPECT_EQ(devices.selected_devices(), *edit.audio_devices);
    undo();
    EXPECT_TRUE(instances.desired_instances().empty());
    EXPECT_EQ(devices.selected_devices(), original);
    EXPECT_FALSE(jit.toolchain_config().cxx_compiler);
    redo();
    EXPECT_EQ(instances.desired_instances().size(), 1u);
    EXPECT_EQ(devices.selected_devices(), *edit.audio_devices);
}

TEST_F(AudioSettingsHistory, StaleToolchainRejectsTheWholeMixedSettingsUndoBeforeDeviceChanges)
{
    auto edit = selection();
    edit.toolchain = iv::ProjectToolchainSettings{.cxx_compiler = "/accepted/compiler"};
    apply(edit);
    auto actual = jit.toolchain_config();
    actual.cxx_compiler = "/external/compiler";
    jit.set_toolchain_config(actual);
    auto const outputs = backend->output_creates;
    auto const inputs = backend->input_creates;
    EXPECT_THROW(undo(), std::runtime_error);
    EXPECT_EQ(backend->output_creates, outputs);
    EXPECT_EQ(backend->input_creates, inputs);
    EXPECT_EQ(devices.selected_devices(), *edit.audio_devices);
    actual.cxx_compiler = edit.toolchain->cxx_compiler;
    jit.set_toolchain_config(actual);
    EXPECT_NO_THROW(undo());
    EXPECT_EQ(devices.selected_devices(), (iv::ProjectAudioDeviceSelection{"default", "default"}));
    EXPECT_FALSE(jit.toolchain_config().cxx_compiler);
}

TEST_F(AudioSettingsHistory, ReplayStartsWithoutAnImpliedSettingsHistory)
{
    iv::NodeInstances instances;
    iv::GraphConnections connections;
    iv::project_graph_node_instances_bridge::scope nodes_scope{graph, instances};
    iv::project_graph_graph_connections_bridge::scope connections_scope{graph, connections};
    apply(selection());
    iv::ProjectAckBuilder replay;
    in_event_source([&] { graph.handle_project_replace_graph({}, replay); });
    ASSERT_NO_THROW(replay.build());
    EXPECT_THROW(undo(), std::runtime_error);
    EXPECT_EQ(devices.selected_devices(), (iv::ProjectAudioDeviceSelection{"out-1", "in-1"}));
}

TEST_F(AudioSettingsHistory, ReplayAppliesExplicitClearsWithoutChangingOmittedSettings)
{
    auto config = jit.toolchain_config();
    config.cxx_compiler = "/before/compiler";
    config.cmake_generator = "Keep this generator";
    jit.set_toolchain_config(config);
    (void)devices.set_selected_devices("out-1", "in-1");
    iv::ProjectOverrideSettingsRequest request{
        .output_device_id = "default",
        .specified_fields = {iv::ProjectSettingField::cxx_compiler,
            iv::ProjectSettingField::output_device_id, iv::ProjectSettingField::input_device_id},
    };
    in_event_source([&] { jit.handle_project_override_settings(request); });
    in_event_source([&] { devices.handle_project_override_settings(request); });
    EXPECT_FALSE(jit.toolchain_config().cxx_compiler);
    EXPECT_EQ(jit.toolchain_config().cmake_generator, config.cmake_generator);
    EXPECT_EQ(devices.selected_devices(), (iv::ProjectAudioDeviceSelection{"default", std::nullopt}));
    EXPECT_THROW(undo(), std::runtime_error); // Replay itself created no history.
}

TEST_F(AudioSettingsHistory, GraphOnlyHistoryDoesNotAskForAudioPermission)
{
    iv::NodeInstances instances;
    iv::GraphConnections connections;
    iv::project_graph_node_instances_bridge::scope nodes_scope{graph, instances};
    iv::project_graph_graph_connections_bridge::scope connections_scope{graph, connections};
    iv::ProjectEditBatch edit;
    edit.instances = {{"pending", iv::ProjectNodeInstanceDeclaration{
        .instance_id = "pending", .definition_id = "missing.definition", .package_root = "/missing/package"}}};
    apply(edit);
    bool asked = false;
    EXPECT_NO_THROW(undo([&] { asked = true; throw std::runtime_error("audio.control denied"); }));
    EXPECT_FALSE(asked);
    EXPECT_TRUE(instances.desired_instances().empty());
}
