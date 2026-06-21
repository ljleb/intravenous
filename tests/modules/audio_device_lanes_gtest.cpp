#include <intravenous/basic_lane_nodes/controls.h>
#include <intravenous/runtime/audio_device_lanes.h>
#include <intravenous/runtime/audio_device_lanes_timeline_bridge.h>
#include <intravenous/runtime/audio_device_lanes_timeline_execution_bridge.h>
#include <intravenous/runtime/audio_input_synchronizer.h>
#include <intravenous/runtime/task_runner.h>
#include <intravenous/runtime/task_runner_audio_device_lanes_bridge.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/runtime/timeline_execution.h>
#include <intravenous/runtime/timeline_timeline_execution_bridge.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace {
using namespace std::chrono_literals;

struct SharedFakeAudioOutputState {
    iv::RenderConfig config {};
    std::mutex mutex {};
    std::condition_variable cv {};
    bool shutdown_requested = false;
    bool request_pending = false;
    bool response_ready = false;
    bool block_ready = false;
    size_t requested_frame_index = 0;
    size_t requested_frames = 0;
    std::vector<iv::Sample> interleaved_output {};
    std::vector<iv::Sample> planar_output {};

    explicit SharedFakeAudioOutputState(iv::RenderConfig config_in) :
        config(config_in),
        interleaved_output(config.max_block_frames * config.num_channels, iv::Sample {}),
        planar_output(config.max_block_frames * config.num_channels, iv::Sample {})
    {
        iv::validate_render_config(config);
    }

    void materialize_planar_output_locked()
    {
        std::fill(planar_output.begin(), planar_output.end(), iv::Sample {});
        for (size_t frame = 0; frame < requested_frames && frame < config.max_block_frames; ++frame) {
            for (size_t channel = 0; channel < config.num_channels; ++channel) {
                planar_output[channel * config.max_block_frames + frame] =
                    interleaved_output[frame * config.num_channels + channel];
            }
        }
    }

    void begin_requested_block(size_t frame_index, size_t frame_count)
    {
        {
            std::scoped_lock lock(mutex);
            requested_frame_index = frame_index;
            requested_frames = frame_count;
            request_pending = true;
            response_ready = false;
            block_ready = false;
            std::fill(interleaved_output.begin(), interleaved_output.end(), iv::Sample {});
            std::fill(planar_output.begin(), planar_output.end(), iv::Sample {});
        }
        cv.notify_all();
    }

    bool wait_until_block_ready_for(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] {
            return shutdown_requested || block_ready;
        }) && !shutdown_requested && block_ready;
    }

    std::span<iv::Sample const> output_block(size_t channel)
    {
        std::scoped_lock lock(mutex);
        return {
            planar_output.data() + channel * config.max_block_frames,
            config.max_block_frames,
        };
    }
};

class SharedFakeAudioOutputDevice {
    std::shared_ptr<SharedFakeAudioOutputState> state_;

public:
    explicit SharedFakeAudioOutputDevice(std::shared_ptr<SharedFakeAudioOutputState> state) :
        state_(std::move(state))
    {}

    iv::RenderConfig const &config() const
    {
        return state_->config;
    }

    std::span<iv::Sample> wait_for_block_request()
    {
        std::unique_lock lock(state_->mutex);
        state_->cv.wait(lock, [&] {
            return state_->shutdown_requested || (state_->request_pending && !state_->response_ready);
        });
        if (!state_->request_pending) {
            throw std::logic_error("shared fake audio output device interrupted without a pending request");
        }
        return std::span<iv::Sample>(
            state_->interleaved_output.data(),
            state_->requested_frames * state_->config.num_channels);
    }

    void submit_response()
    {
        {
            std::scoped_lock lock(state_->mutex);
            state_->materialize_planar_output_locked();
            state_->response_ready = true;
            state_->block_ready = true;
        }
        state_->cv.notify_all();
    }

    void request_shutdown()
    {
        {
            std::scoped_lock lock(state_->mutex);
            state_->shutdown_requested = true;
        }
        state_->cv.notify_all();
    }
};

class IdleFakeAudioInputDevice {
    iv::RenderConfig config_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool shutdown_requested_ = false;
    std::vector<iv::Sample> storage_;

public:
    explicit IdleFakeAudioInputDevice(iv::RenderConfig config)
        : config_(std::move(config)),
          storage_(config_.max_block_frames * config_.num_channels, iv::Sample {})
    {
        iv::validate_render_config(config_);
    }

    iv::RenderConfig const &config() const
    {
        return config_;
    }

    iv::AudioInputBlock wait_for_captured_block()
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] {
            return shutdown_requested_;
        });
        throw std::logic_error("idle fake input device interrupted");
    }

    void release_captured_block() {}

    void request_shutdown()
    {
        {
            std::scoped_lock lock(mutex_);
            shutdown_requested_ = true;
        }
        cv_.notify_all();
    }
};

iv::AudioDeviceLanesBackend make_test_audio_backend(
    std::shared_ptr<SharedFakeAudioOutputState> output_state)
{
    return iv::AudioDeviceLanesBackend{
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
        .make_output_device = [output_state](std::string const &, iv::RenderConfig const &) {
            return iv::AudioOutputDevice(
                std::in_place_type<SharedFakeAudioOutputDevice>,
                output_state);
        },
        .make_input_device = [](std::string const &, iv::RenderConfig const &config) {
            return iv::AudioInputDevice(
                std::in_place_type<IdleFakeAudioInputDevice>,
                config);
        },
    };
}

std::optional<iv::LaneId> find_lane_with_unit(iv::Timeline &timeline, std::string_view key)
{
    return timeline.with_graph([&](iv::LaneGraph const &graph) -> std::optional<iv::LaneId> {
        std::optional<iv::LaneId> result;
        graph.for_each_lane([&](iv::LaneRecord const &lane) {
            if (lane.metadata.has_unit(key)) {
                result = lane.id;
            }
        });
        return result;
    });
}

TEST(AudioInputSynchronizer, PullsRateTowardTargetBufferDepth)
{
    iv::AudioInputSynchronizer too_full(48000, 64, 48000, 128);
    std::vector<iv::Sample> captured(512 * 2, iv::Sample {0.25f});
    too_full.push_captured_block(iv::AudioInputBlock{
        .samples = captured,
        .capture_timestamp_seconds = 0.0,
        .discontinuous = false,
    });
    auto full_block = too_full.render_timeline_block();
    EXPECT_EQ(full_block.channel_layout, (iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::interleaved,
    }));
    EXPECT_EQ(full_block.frame_count, 64u);
    EXPECT_GT(too_full.last_rate_ratio(), 1.0);

    iv::AudioInputSynchronizer too_empty(48000, 64, 48000, 256);
    std::vector<iv::Sample> sparse(64 * 2, iv::Sample {0.5f});
    too_empty.push_captured_block(iv::AudioInputBlock{
        .samples = sparse,
        .capture_timestamp_seconds = 0.0,
        .discontinuous = false,
    });
    auto empty_block = too_empty.render_timeline_block();
    EXPECT_EQ(empty_block.frame_count, 64u);
    EXPECT_LT(too_empty.last_rate_ratio(), 1.0);
}

TEST(AudioDeviceLanes, RendersTimelineAudioIntoOutputDeviceRequests)
{
    constexpr size_t kBlockSize = 64;
    iv::Timeline timeline;
    iv::TimelineExecution execution(kBlockSize, 1);
    std::optional<iv::TasksRunner> runner;
    runner.emplace(1);

    auto output_state = std::make_shared<SharedFakeAudioOutputState>(iv::RenderConfig{
        .sample_rate = 48000,
        .num_channels = 2,
        .max_block_frames = kBlockSize,
        .preferred_block_size = kBlockSize,
    });

    iv::AudioDeviceLanes audio_device_lanes(
        48000,
        kBlockSize,
        make_test_audio_backend(output_state),
        std::optional<std::string>("default"),
        std::nullopt);

    iv::bind_audio_device_lanes_timeline_bridge(audio_device_lanes, timeline);
    iv::bind_audio_device_lanes_timeline_execution_bridge(audio_device_lanes, execution);
    iv::bind_task_runner_audio_device_lanes_bridge(audio_device_lanes);
    iv::bind_timeline_timeline_execution_bridge(timeline, execution);

    audio_device_lanes.bind();

    auto const output_lane = find_lane_with_unit(timeline, "audio_output");
    ASSERT_TRUE(output_lane.has_value());

    iv::TimelineLaneBatchUpdate batch;
    batch.version_index = 2;
    batch.upserts.push_back(iv::TimelineLaneUpsert{
        .lane = iv::LaneId{1000},
        .make_node = [] {
            return iv::TypeErasedLaneNode(iv::KnobLaneNode{
                .value = 0.25f,
            });
        },
        .sample_channel_type = iv::ChannelTypeId::mono,
    });
    batch.connections_to_add.push_back(iv::LaneGraphConnection{
        .source = iv::LaneId{1000},
        .target = *output_lane,
        .input = iv::realtime_sample_input(0),
    });
    timeline.apply_lane_batch(batch);

    auto update = timeline.with_graph([&](iv::LaneGraph const &graph) {
        return execution.synchronize_from_graph(graph);
    });
    runner->update_tasks(update);

    output_state->begin_requested_block(0, kBlockSize);
    ASSERT_TRUE(output_state->wait_until_block_ready_for(2s));

    auto const left = output_state->output_block(0);
    auto const right = output_state->output_block(1);
    ASSERT_EQ(left.size(), kBlockSize);
    ASSERT_EQ(right.size(), kBlockSize);
    for (size_t i = 0; i < kBlockSize; ++i) {
        EXPECT_NEAR(static_cast<float>(left[i]), 0.25f, 1e-5f);
        EXPECT_NEAR(static_cast<float>(right[i]), 0.25f, 1e-5f);
    }

    audio_device_lanes.request_shutdown();
    iv::unbind_task_runner_audio_device_lanes_bridge(audio_device_lanes);
    runner.reset();
    iv::unbind_timeline_timeline_execution_bridge(timeline, execution);
    iv::unbind_audio_device_lanes_timeline_execution_bridge(audio_device_lanes, execution);
    iv::unbind_audio_device_lanes_timeline_bridge(audio_device_lanes, timeline);
}

TEST(AudioDeviceLanes, ReportsInventoryAndUnavailableSelections)
{
    auto output_state = std::make_shared<SharedFakeAudioOutputState>(iv::RenderConfig{
        .sample_rate = 48000,
        .num_channels = 2,
        .max_block_frames = 64,
        .preferred_block_size = 64,
    });

    iv::AudioDeviceLanes audio_device_lanes(
        48000,
        64,
        make_test_audio_backend(output_state),
        std::optional<std::string>("default"),
        std::nullopt);

    auto snapshot = audio_device_lanes.audio_devices_snapshot();
    EXPECT_EQ(snapshot.output_devices.size(), 2u);
    EXPECT_EQ(snapshot.input_devices.size(), 2u);
    ASSERT_TRUE(snapshot.selected_output.device_id.has_value());
    EXPECT_EQ(*snapshot.selected_output.device_id, "default");
    EXPECT_TRUE(snapshot.selected_output.available);
    EXPECT_FALSE(snapshot.selected_input.device_id.has_value());
    EXPECT_FALSE(snapshot.selected_input.available);

    snapshot = audio_device_lanes.set_selected_devices(
        std::optional<std::string>("missing-output"),
        std::optional<std::string>("in-1"));
    ASSERT_TRUE(snapshot.selected_output.device_id.has_value());
    EXPECT_EQ(*snapshot.selected_output.device_id, "missing-output");
    EXPECT_FALSE(snapshot.selected_output.available);
    ASSERT_TRUE(snapshot.selected_input.device_id.has_value());
    EXPECT_EQ(*snapshot.selected_input.device_id, "in-1");
    EXPECT_TRUE(snapshot.selected_input.available);

    audio_device_lanes.request_shutdown();
}

} // namespace
