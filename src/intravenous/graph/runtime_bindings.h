#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/lane_id.h>
#include <intravenous/ports.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace iv {

enum class RuntimeSampleInputMode : std::uint8_t {
    none,
    scalar,
    timeline,
};

using RuntimeTimelineSampleBlockReader = void (*)(
    LaneId timeline_lane,
    size_t source_channel,
    size_t source_channel_offset,
    size_t block_index,
    size_t block_size,
    ChannelLayout target_layout,
    std::span<Sample> target);

struct RuntimeEventBlockView {
    TimedEvent const* data = nullptr;
    size_t size = 0;
};

using RuntimeTimelineEventBlockReader = RuntimeEventBlockView (*)(
    LaneId timeline_lane,
    size_t block_index,
    size_t block_size);

using RuntimeSampleBlockPublisher = void (*)(
    LaneId target_lane,
    std::span<Sample const> samples,
    ChannelLayout layout,
    size_t frame_count);

using RuntimeEventBlockPublisher = void (*)(
    LaneId target_lane,
    std::span<TimedEvent const> events);

struct RuntimeSampleInputBinding {
    RuntimeSampleInputMode mode = RuntimeSampleInputMode::none;
    Sample value = Sample{0};
    LaneId timeline_lane {};
    size_t source_channel = 0;
    RuntimeTimelineSampleBlockReader read_timeline_block = nullptr;
};

struct RuntimeEventInputBinding {
    LaneId timeline_lane {};
    RuntimeTimelineEventBlockReader read_timeline_block = nullptr;
};

struct RuntimeOutputBinding {
    LaneId target_lane {};
    bool include_in_aggregate = false;
    RuntimeSampleBlockPublisher publish_sample_block = nullptr;
    RuntimeEventBlockPublisher publish_event_block = nullptr;
};

class GraphRuntimeBindings {
public:
    struct Callbacks {
        RuntimeTimelineSampleBlockReader read_timeline_sample_block;
        RuntimeTimelineEventBlockReader read_timeline_event_block;
        RuntimeSampleBlockPublisher publish_sample_block;
        RuntimeEventBlockPublisher publish_event_block;
    };

    GraphRuntimeBindings()
      : GraphRuntimeBindings(Callbacks{})
    {}

    explicit GraphRuntimeBindings(Callbacks callbacks)
      : callbacks_(callbacks)
    {}

    std::shared_ptr<RuntimeSampleInputBinding> sample_input(
        std::string const& semantic_key)
    {
        std::scoped_lock lock(mutex_);
        auto& binding = sample_inputs_[semantic_key];
        if (!binding) {
            binding = std::make_shared<RuntimeSampleInputBinding>();
            binding->read_timeline_block =
                callbacks_.read_timeline_sample_block;
        }
        return binding;
    }

    std::shared_ptr<RuntimeEventInputBinding> event_input(
        std::string const& semantic_key)
    {
        std::scoped_lock lock(mutex_);
        auto& binding = event_inputs_[semantic_key];
        if (!binding) {
            binding = std::make_shared<RuntimeEventInputBinding>();
            binding->read_timeline_block =
                callbacks_.read_timeline_event_block;
        }
        return binding;
    }

    std::shared_ptr<RuntimeOutputBinding> output(
        std::string const& semantic_key)
    {
        std::scoped_lock lock(mutex_);
        auto& binding = outputs_[semantic_key];
        if (!binding) {
            binding = std::make_shared<RuntimeOutputBinding>();
            binding->publish_sample_block = callbacks_.publish_sample_block;
            binding->publish_event_block = callbacks_.publish_event_block;
        }
        return binding;
    }

private:
    Callbacks callbacks_ {};
    std::mutex mutex_ {};
    std::unordered_map<std::string, std::shared_ptr<RuntimeSampleInputBinding>>
        sample_inputs_ {};
    std::unordered_map<std::string, std::shared_ptr<RuntimeEventInputBinding>>
        event_inputs_ {};
    std::unordered_map<std::string, std::shared_ptr<RuntimeOutputBinding>>
        outputs_ {};
};

inline std::string runtime_virtual_port_key(
    bool input,
    PortKind kind,
    std::string_view virtual_node_id,
    std::optional<size_t> member_ordinal,
    size_t port_ordinal)
{
    return std::string(input ? "input/" : "output/")
        + (kind == PortKind::sample ? "sample/" : "event/")
        + std::string(virtual_node_id) + "/"
        + (member_ordinal ? std::to_string(*member_ordinal) : "virtual") + "/"
        + std::to_string(port_ordinal);
}

inline std::string runtime_public_port_key(
    bool input,
    PortKind kind,
    size_t port_ordinal)
{
    return std::string(input ? "public-input/" : "public-output/")
        + (kind == PortKind::sample ? "sample/" : "event/")
        + std::to_string(port_ordinal);
}

} // namespace iv
