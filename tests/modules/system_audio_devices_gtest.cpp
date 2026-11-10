#include <intravenous/runtime/audio_input_synchronizer.h>
#include <intravenous/runtime/graph_executor.h>
#include <intravenous/runtime/system_audio_devices.h>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
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
