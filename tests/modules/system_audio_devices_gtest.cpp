#include <intravenous/runtime/audio_input_synchronizer.h>
#include <intravenous/runtime/system_audio_devices.h>

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {
struct DeviceLifetimeState {
    std::atomic<size_t> output_shutdowns = 0;
    std::atomic<size_t> input_shutdowns = 0;
};

class FakeOutputDevice {
    iv::RenderConfig config_;
    std::shared_ptr<DeviceLifetimeState> lifetime_;
    std::vector<iv::Sample> buffer_;

public:
    FakeOutputDevice(iv::RenderConfig config, std::shared_ptr<DeviceLifetimeState> lifetime)
        : config_(config)
        , lifetime_(std::move(lifetime))
        , buffer_(config.max_block_frames * config.num_channels)
    {}

    iv::RenderConfig const& config() const { return config_; }
    std::span<iv::Sample> wait_for_block_request() { return buffer_; }
    void submit_response() {}
    void request_shutdown() { ++lifetime_->output_shutdowns; }
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
}
} // namespace
