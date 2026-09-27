#include <intravenous/runtime/background_evaluation_call_frame.h>

#include <intravenous/channel_layout.h>
#include <intravenous/compat.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace iv {
namespace {

[[nodiscard]] bool contains_coverage(Coverage const& outer,
                                     Coverage const& inner) noexcept
{
    return std::ranges::all_of(inner.regions(), [&](IndexRegion region) {
        return outer.contains(region);
    });
}

[[nodiscard]] Coverage shifted_coverage(Coverage const& source,
                                        std::size_t latency)
{
    Coverage result;
    for (auto const region : source.regions()) {
        auto const begin = saturating_sample_index_add(region.begin, latency);
        auto const end = saturating_sample_index_add(region.end, latency);
        if (begin < end)
            result.include({begin, end});
    }
    return result;
}

[[nodiscard]] bool valid_replay_sample_storage(
    ReflectedSamplePortStorageBinding const& storage) noexcept
{
    if (!is_valid_channel_type(storage.channel_layout.channel_type) ||
        !is_valid_sample_stream_layout(storage.channel_layout.sample_layout) ||
        storage.frame_capacity == 0 ||
        !std::has_single_bit(storage.frame_capacity)) {
        return false;
    }
    auto const count = channel_count(storage.channel_layout);
    for (std::size_t channel = 0; channel < count; ++channel) {
        auto const& binding = storage.channels[channel];
        if (binding.storage == nullptr || binding.frame_capacity == 0 ||
            !std::has_single_bit(binding.frame_capacity) ||
            binding.frame_stride == 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_replay_event_storage(
    ReflectedEventPortStorageBinding const& storage) noexcept
{
    return storage.storage != nullptr && storage.event_capacity != 0 &&
           std::has_single_bit(storage.event_capacity) &&
           static_cast<std::size_t>(storage.type) <
               static_cast<std::size_t>(EventTypeId::count);
}

template <typename Value>
void append_unique(std::vector<Value>& values, Value value)
{
    if (!std::ranges::contains(values, value)) {
        values.push_back(std::move(value));
    }
}

template <typename View>
void append_unique_view(std::vector<View>& views, View view)
{
    auto const duplicate = std::ranges::any_of(
        views, [&](View const& other) { return other.data == view.data; });
    if (!duplicate)
        views.push_back(std::move(view));
}

[[nodiscard]] std::size_t replay_sample_count(
    std::span<IndexRegion const> regions)
{
    std::size_t result = 0;
    for (auto const region : regions) {
        auto const size = region.end - region.begin;
        if (size > std::numeric_limits<std::size_t>::max() - result) {
            throw std::length_error("background replay schedule is too large");
        }
        result += static_cast<std::size_t>(size);
    }
    return result;
}

[[nodiscard]] std::size_t replay_frame_capacity(
    std::span<IndexRegion const> regions)
{
    if (regions.empty()) return 0;
    auto const span = regions.back().end - regions.front().begin;
    auto const maximum_power = std::size_t{1}
        << (std::numeric_limits<std::size_t>::digits - 1);
    if (span > maximum_power) {
        throw std::length_error(
            "background replay raw sample span is too large");
    }
    return std::bit_ceil(std::max<std::size_t>(span, 1));
}

[[nodiscard]] std::size_t replay_event_capacity(
    double maximum_per_index, std::span<IndexRegion const> regions)
{
    auto requested = event_count_for_sample_span(
        maximum_per_index, replay_sample_count(regions));
    if (!requested) {
        throw std::length_error(
            "background replay raw event capacity is not representable");
    }
    auto const count = std::max<std::size_t>(*requested, 1);
    auto const maximum_power = std::size_t{1}
        << (std::numeric_limits<std::size_t>::digits - 1);
    if (count > maximum_power) {
        throw std::length_error(
            "background replay raw event capacity is too large");
    }
    return std::bit_ceil(count);
}

[[nodiscard]] std::size_t align_up(
    std::size_t value, std::size_t alignment)
{
    auto const remainder = value % alignment;
    if (remainder == 0) return value;
    auto const padding = alignment - remainder;
    if (value > std::numeric_limits<std::size_t>::max() - padding) {
        throw std::length_error("background replay raw storage is too large");
    }
    return value + padding;
}

} // namespace

struct BackgroundEvaluationCallFrame::BindingAdapter {
    struct SampleReadRoute {
        BackgroundSampleReadView view{};
        std::size_t logical_channel = 0;
        std::size_t storage_channel = 0;
        std::size_t read_latency = 0;

        bool operator==(SampleReadRoute const& other) const noexcept
        {
            return view.data == other.view.data &&
                   logical_channel == other.logical_channel &&
                   storage_channel == other.storage_channel &&
                   read_latency == other.read_latency;
        }
    };

    Coverage const* coverage = nullptr;
    std::vector<SampleReadRoute> sample_reads{};
    std::vector<BackgroundSampleWriteView> sample_writes{};
    std::vector<BackgroundEventReadView> event_reads{};
    std::vector<BackgroundEventWriteView> event_writes{};
    bool rejected_write = false;

    static Sample read_sample(void const* opaque, SampleIndex index,
                              std::size_t channel) noexcept
    {
        auto const& self = *static_cast<BindingAdapter const*>(opaque);
        for (auto const& route : self.sample_reads) {
            if (route.logical_channel != channel || index < route.read_latency)
                continue;
            auto const source_index = index - route.read_latency;
            if (route.view.coverage().contains(source_index)) {
                return route.view.at(source_index, route.storage_channel);
            }
        }
        IV_ASSERT(false,
                  "background sample binding has no selected readable route");
        return {};
    }

    static void write_sample(void* opaque, SampleIndex index,
                             std::size_t channel, Sample value) noexcept
    {
        auto& self = *static_cast<BindingAdapter*>(opaque);
        bool routed = false;
        bool rejected = false;
        for (auto const& view : self.sample_writes) {
            if (!view.has_channel(channel) || !view.coverage().contains(index))
                continue;
            routed = true;
            rejected = !view.write(index, channel, value) || rejected;
        }
        self.rejected_write = self.rejected_write || !routed || rejected;
        IV_ASSERT(routed && !rejected,
                  "background sample binding has no selected writable route");
    }

    static void for_each_event(void const* opaque, SampleIndex begin,
                               SampleIndex end, void* visitor_data,
                               RandomAccessEventInputPort::VisitEvent visitor)
    {
        auto const& self = *static_cast<BindingAdapter const*>(opaque);
        auto cursor = begin;
        while (cursor < end) {
            BackgroundEventReadView const* selected = nullptr;
            SampleIndex selected_end = cursor;
            for (auto const& view : self.event_reads) {
                for (auto const region : view.coverage().regions()) {
                    if (!region.contains(cursor))
                        continue;
                    selected = &view;
                    selected_end = std::min(end, region.end);
                    break;
                }
                if (selected != nullptr)
                    break;
            }
            IV_ASSERT(
                selected != nullptr && cursor < selected_end,
                "background event binding has no selected readable route");
            if (selected == nullptr || cursor >= selected_end)
                return;
            selected->for_each_event(selected->data, {cursor, selected_end},
                                     visitor_data, visitor);
            cursor = selected_end;
        }
    }

    static void write_event(void* opaque, TimedEvent const& event) noexcept
    {
        auto& self = *static_cast<BindingAdapter*>(opaque);
        bool routed = false;
        bool rejected = false;
        for (auto const& view : self.event_writes) {
            if (!view.coverage().contains(event.time))
                continue;
            routed = true;
            rejected = !view.write(event) || rejected;
        }
        self.rejected_write = self.rejected_write || !routed || rejected;
        IV_ASSERT(routed && !rejected,
                  "background event binding has no selected writable route");
    }
};

struct BackgroundEvaluationCallFrame::NodeFrameStorage {
    std::vector<RandomAccessSampleInputPort> sample_inputs{};
    std::vector<TockSampleOutputPort> sample_outputs{};
    std::vector<RandomAccessEventInputPort> event_inputs{};
    std::vector<TockEventOutputPort> event_outputs{};
};

struct BackgroundEvaluationCallFrame::ReplayFrameStorage {
    struct SampleStorage {
        ChannelLayout layout{};
        std::size_t capacity = 0;
        std::vector<Sample> values{};

        SampleStorage(ChannelLayout selected_layout, std::size_t selected_capacity)
            : layout(selected_layout), capacity(selected_capacity)
        {
            auto const channels = channel_count(layout);
            if (capacity != 0
                && channels > std::numeric_limits<std::size_t>::max()
                    / capacity) {
                throw std::length_error(
                    "background replay raw sample storage is too large");
            }
            values.resize(channels * capacity);
        }

        [[nodiscard]] ReflectedSamplePortStorageBinding binding() noexcept
        {
            ReflectedSamplePortStorageBinding result{
                .frame_capacity = capacity,
                .channel_layout = layout,
            };
            auto const channels = channel_count(layout);
            for (std::size_t channel = 0; channel < channels; ++channel) {
                auto const offset = layout.sample_layout
                        == SampleStreamLayout::planar
                    ? channel * capacity
                    : channel;
                result.channels[channel] = {
                    .storage = reinterpret_cast<std::byte*>(
                        values.data() + offset),
                    .frame_capacity = capacity,
                    .frame_stride = layout.sample_layout
                            == SampleStreamLayout::planar
                        ? 1
                        : channels,
                };
            }
            return result;
        }

        [[nodiscard]] Sample& at(
            SampleIndex index, std::size_t channel) noexcept
        {
            auto const frame = static_cast<std::size_t>(index) & (capacity - 1);
            auto const channels = channel_count(layout);
            auto const offset = layout.sample_layout
                    == SampleStreamLayout::planar
                ? channel * capacity + frame
                : frame * channels + channel;
            return values[offset];
        }
    };

    struct EventStorage {
        std::size_t events_offset = 0;
        std::size_t capacity = 0;
        std::vector<std::max_align_t> words{};
        std::uint64_t overflow = 0;

        explicit EventStorage(std::size_t selected_capacity)
            : events_offset(align_up(sizeof(std::size_t), alignof(TimedEvent)))
            , capacity(selected_capacity)
        {
            static_assert(alignof(TimedEvent) <= alignof(std::max_align_t));
            if (capacity > (std::numeric_limits<std::size_t>::max()
                    - events_offset) / sizeof(TimedEvent)) {
                throw std::length_error(
                    "background replay raw event storage is too large");
            }
            auto const bytes = events_offset + capacity * sizeof(TimedEvent);
            if (bytes > std::numeric_limits<std::size_t>::max()
                    - (sizeof(std::max_align_t) - 1)) {
                throw std::length_error(
                    "background replay raw event storage is too large");
            }
            words.resize((bytes + sizeof(std::max_align_t) - 1)
                         / sizeof(std::max_align_t));
            count() = 0;
        }

        [[nodiscard]] std::byte* data() noexcept
        {
            return reinterpret_cast<std::byte*>(words.data());
        }
        [[nodiscard]] std::size_t& count() noexcept
        {
            return *reinterpret_cast<std::size_t*>(data());
        }
        [[nodiscard]] TimedEvent* events() noexcept
        {
            return reinterpret_cast<TimedEvent*>(data() + events_offset);
        }
        [[nodiscard]] ReflectedEventPortStorageBinding binding(
            EventTypeId type) noexcept
        {
            return {
                .storage = data(),
                .count_offset = 0,
                .events_offset = events_offset,
                .event_capacity = capacity,
                .type = type,
            };
        }
    };

    std::vector<ReflectedSampleInputPortBinding> sample_inputs{};
    std::vector<ReflectedSampleOutputPortBinding> sample_outputs{};
    std::vector<ReflectedEventInputPortBinding> event_inputs{};
    std::vector<ReflectedEventOutputPortBinding> event_outputs{};
    std::vector<IndexRegion> regions{};
    std::vector<std::unique_ptr<SampleStorage>> sample_input_storage{};
    std::vector<std::unique_ptr<SampleStorage>> sample_output_storage{};
    std::vector<std::unique_ptr<EventStorage>> event_input_storage{};
    std::vector<std::unique_ptr<EventStorage>> event_output_storage{};
    std::vector<BindingAdapter*> sample_input_adapters{};
    std::vector<BindingAdapter*> sample_output_adapters{};
    std::vector<BindingAdapter*> event_input_adapters{};
    std::vector<BindingAdapter*> event_output_adapters{};
};

void BackgroundEvaluationCallFrame::prepare_replay(void* opaque)
{
    auto& replay = *static_cast<ReplayFrameStorage*>(opaque);
    for (std::size_t input = 0;
         input < replay.sample_input_storage.size(); ++input) {
        auto& raw = *replay.sample_input_storage[input];
        auto const& adapter = *replay.sample_input_adapters[input];
        for (auto const region : replay.regions) {
            if (!adapter.coverage->contains(region)) {
                throw std::runtime_error(
                    "background replay sample input is not fully selected");
            }
            for (auto index = region.begin; index < region.end; ++index) {
                for (std::size_t channel = 0;
                     channel < channel_count(raw.layout); ++channel) {
                    raw.at(index, channel) =
                        BindingAdapter::read_sample(&adapter, index, channel);
                }
            }
        }
    }
    for (std::size_t input = 0;
         input < replay.event_input_storage.size(); ++input) {
        auto& raw = *replay.event_input_storage[input];
        auto const& adapter = *replay.event_input_adapters[input];
        raw.count() = 0;
        for (auto const region : replay.regions) {
            if (!adapter.coverage->contains(region)) {
                throw std::runtime_error(
                    "background replay event input is not fully selected");
            }
            BindingAdapter::for_each_event(
                &adapter, region.begin, region.end, &raw,
                +[](void* data, TimedEvent const& event) {
                    auto& storage = *static_cast<
                        ReplayFrameStorage::EventStorage*>(data);
                    if (storage.count() == storage.capacity) {
                        throw std::runtime_error(
                            "background replay event input exceeded its bound");
                    }
                    storage.events()[storage.count()++] = event;
                });
        }
    }
    for (auto& output : replay.event_output_storage) {
        output->count() = 0;
        output->overflow = 0;
    }
}

void BackgroundEvaluationCallFrame::finalize_replay(void* opaque)
{
    auto& replay = *static_cast<ReplayFrameStorage*>(opaque);
    for (std::size_t output = 0;
         output < replay.sample_output_storage.size(); ++output) {
        auto& raw = *replay.sample_output_storage[output];
        auto& adapter = *replay.sample_output_adapters[output];
        Coverage scheduled;
        for (auto const region : replay.regions) scheduled.include(region);
        auto const selected = scheduled & *adapter.coverage;
        for (auto const region : selected.regions()) {
            for (auto index = region.begin; index < region.end; ++index) {
                for (std::size_t channel = 0;
                     channel < channel_count(raw.layout); ++channel) {
                    BindingAdapter::write_sample(
                        &adapter, index, channel, raw.at(index, channel));
                }
            }
        }
    }
    for (std::size_t output = 0;
         output < replay.event_output_storage.size(); ++output) {
        auto& raw = *replay.event_output_storage[output];
        auto& adapter = *replay.event_output_adapters[output];
        if (raw.overflow != 0) {
            throw std::runtime_error(
                "background replay event output exceeded its bound");
        }
        for (std::size_t event = 0; event < raw.count(); ++event) {
            auto const& value = raw.events()[event];
            if (adapter.coverage->contains(
                    static_cast<SampleIndex>(value.time))) {
                BindingAdapter::write_event(&adapter, value);
            }
        }
    }
}

BackgroundEvaluationCallFrame::BackgroundEvaluationCallFrame(
    graph_jit::BackgroundEvaluationPlan const& plan,
    BackgroundStorageRealization& realization,
    BackgroundEvaluationCallFrameSelection selection)
    : plan_(&plan), realization_(&realization),
      selection_(std::move(selection)), nodes_(plan.nodes.size())
{
    replay_storage_.reserve(plan.runtime.replay_invocations.size());
    for (auto const& replay : plan.runtime.replay_invocations) {
        auto storage = std::make_unique<ReplayFrameStorage>();
        for (auto const slot : replay.input_bindings) {
            if (slot >= plan.runtime.bindings.size()) {
                throw std::invalid_argument(
                    "background replay input binding slot is out of range");
            }
            auto const& binding = plan.runtime.bindings[slot];
            if (binding.kind == PortKind::sample) {
                storage->sample_inputs.emplace_back();
            } else {
                storage->event_inputs.emplace_back();
            }
        }
        for (auto const slot : replay.output_bindings) {
            if (slot >= plan.runtime.bindings.size()) {
                throw std::invalid_argument(
                    "background replay output binding slot is out of range");
            }
            auto const& binding = plan.runtime.bindings[slot];
            if (binding.kind == PortKind::sample) {
                storage->sample_outputs.emplace_back();
            } else {
                storage->event_outputs.emplace_back();
            }
        }
        replay_storage_.push_back(std::move(storage));
    }
}

BackgroundEvaluationCallFrame::~BackgroundEvaluationCallFrame() = default;

std::span<ReflectedSampleInputPortBinding>
BackgroundEvaluationCallFrame::replay_sample_inputs(
    graph_jit::BackgroundReplayInvocationSlot slot)
{
    if (sealed_) {
        throw std::logic_error("background call frame is already sealed");
    }
    if (slot >= replay_storage_.size()) {
        throw std::out_of_range("background replay slot is out of range");
    }
    return replay_storage_[slot]->sample_inputs;
}

std::span<ReflectedSampleOutputPortBinding>
BackgroundEvaluationCallFrame::replay_sample_outputs(
    graph_jit::BackgroundReplayInvocationSlot slot)
{
    if (sealed_) {
        throw std::logic_error("background call frame is already sealed");
    }
    if (slot >= replay_storage_.size()) {
        throw std::out_of_range("background replay slot is out of range");
    }
    return replay_storage_[slot]->sample_outputs;
}

std::span<ReflectedEventInputPortBinding>
BackgroundEvaluationCallFrame::replay_event_inputs(
    graph_jit::BackgroundReplayInvocationSlot slot)
{
    if (sealed_) {
        throw std::logic_error("background call frame is already sealed");
    }
    if (slot >= replay_storage_.size()) {
        throw std::out_of_range("background replay slot is out of range");
    }
    return replay_storage_[slot]->event_inputs;
}

std::span<ReflectedEventOutputPortBinding>
BackgroundEvaluationCallFrame::replay_event_outputs(
    graph_jit::BackgroundReplayInvocationSlot slot)
{
    if (sealed_) {
        throw std::logic_error("background call frame is already sealed");
    }
    if (slot >= replay_storage_.size()) {
        throw std::out_of_range("background replay slot is out of range");
    }
    return replay_storage_[slot]->event_outputs;
}

std::expected<void, std::string>
BackgroundEvaluationCallFrame::set_replay_regions(
    graph_jit::BackgroundReplayInvocationSlot slot,
    std::vector<IndexRegion> regions)
{
    if (sealed_) {
        return std::unexpected("background call frame is already sealed");
    }
    if (slot >= replay_storage_.size() ||
        slot >= plan_->runtime.replay_invocations.size()) {
        return std::unexpected("background replay slot is out of range");
    }
    auto const maximum =
        plan_->runtime.replay_invocations[slot].maximum_block_size;
    SampleIndex previous_end = 0;
    bool first = true;
    for (auto const region : regions) {
        if (!region.valid() || region.empty() ||
            region.end - region.begin > maximum) {
            return std::unexpected(
                "background replay region violates its compiled block limit");
        }
        if (!first && region.begin < previous_end) {
            return std::unexpected(
                "background replay regions are not in invocation order");
        }
        first = false;
        previous_end = region.end;
    }
    replay_storage_[slot]->regions = std::move(regions);
    return {};
}

std::expected<void, std::string> BackgroundEvaluationCallFrame::seal()
{
    if (sealed_) {
        return std::unexpected("background call frame is already sealed");
    }
    auto const& plan = *plan_;
    auto const& runtime = plan.runtime;
    if (!realization_->sealed()) {
        return std::unexpected(
            "background call frame requires a sealed storage realization");
    }
    if (selection_.sample_rate == 0) {
        return std::unexpected(
            "background call frame has an invalid sample rate");
    }
    if (selection_.binding_coverage.size() != runtime.bindings.size()) {
        return std::unexpected(
            "background binding coverage is not aligned with the runtime plan");
    }
    if (runtime.port_bindings.size() != plan.ports.size() ||
        runtime.node_operations.size() != plan.nodes.size() ||
        runtime.node_replay_invocations.size() != plan.nodes.size() ||
        replay_storage_.size() != runtime.replay_invocations.size()) {
        return std::unexpected(
            "background call-frame maps are not aligned with the runtime plan");
    }

    std::vector<std::unique_ptr<BindingAdapter>> adapters;
    adapters.reserve(runtime.bindings.size());
    for (graph_jit::BackgroundBindingSlot slot = 0;
         slot < runtime.bindings.size(); ++slot) {
        auto const& binding = runtime.bindings[slot];
        if (binding.node >= plan.nodes.size() ||
            binding.port >= plan.ports.size() ||
            runtime.port_bindings[binding.port] != slot) {
            return std::unexpected(
                "background runtime binding has an invalid logical port");
        }
        auto const& port = plan.ports[binding.port];
        if (port.node != binding.node || port.kind != binding.kind ||
            port.direction != binding.direction || binding.storage.empty()) {
            return std::unexpected(
                "background runtime binding does not match its logical port");
        }

        auto adapter = std::make_unique<BindingAdapter>();
        adapter->coverage = &selection_.binding_coverage[slot];
        if (adapter->coverage->empty()) {
            adapters.push_back(std::move(adapter));
            continue;
        }
        auto storage_selected = [&](graph_jit::PortStorageIndex index) {
            return std::ranges::contains(binding.storage, index);
        };
        if (binding.kind == PortKind::sample &&
            binding.direction == graph_jit::PortDirection::input) {
            for (auto const& direct : plan.storage.direct_samples) {
                if (!storage_selected(direct.storage) ||
                    direct.target_subset >= plan.sample_target_subsets.size())
                    continue;
                auto const& target =
                    plan.sample_target_subsets[direct.target_subset];
                if (target.port != port.configured_port)
                    continue;
                auto const* view = realization_->sample_read(direct.storage);
                if (view == nullptr) {
                    return std::unexpected("background sample input has no "
                                           "readable direct storage");
                }
                append_unique(adapter->sample_reads,
                              BindingAdapter::SampleReadRoute{
                                  .view = *view,
                                  .logical_channel = direct.target_channel,
                                  .storage_channel = direct.source_channel,
                                  .read_latency = direct.read_latency,
                              });
            }
            for (auto const storage_index : binding.storage) {
                if (storage_index >= plan.storage.ports.size()) {
                    return std::unexpected(
                        "background sample input storage is out of range");
                }
                auto const& storage = plan.storage.ports[storage_index];
                auto const* view = realization_->sample_read(storage_index);
                if (view == nullptr) {
                    return std::unexpected(
                        "background sample input has no readable storage");
                }
                for (auto const subset_index : storage.target_subsets) {
                    if (subset_index >= plan.sample_target_subsets.size()) {
                        return std::unexpected(
                            "background sample target subset is out of range");
                    }
                    auto const& target =
                        plan.sample_target_subsets[subset_index];
                    if (target.port != port.configured_port)
                        continue;
                    for (auto const channel : target.channels) {
                        if (!view->has_channel(channel.channel))
                            continue;
                        append_unique(adapter->sample_reads,
                                      BindingAdapter::SampleReadRoute{
                                          .view = *view,
                                          .logical_channel = channel.channel,
                                          .storage_channel = channel.channel,
                                      });
                    }
                }
            }

            for (std::size_t channel = 0;
                 channel < channel_count(port.sample_layout); ++channel) {
                Coverage available;
                for (auto const& route : adapter->sample_reads) {
                    if (route.logical_channel == channel) {
                        available.include(shifted_coverage(
                            route.view.coverage(), route.read_latency));
                    }
                }
                if (!contains_coverage(available, *adapter->coverage)) {
                    return std::unexpected("background sample input coverage "
                                           "has no complete route");
                }
            }
        } else if (binding.kind == PortKind::sample) {
            for (auto const storage_index : binding.storage) {
                if (storage_index >= plan.storage.ports.size()) {
                    return std::unexpected(
                        "background sample output storage is out of range");
                }
                auto const* view = realization_->sample_write(storage_index);
                if (view == nullptr) {
                    return std::unexpected(
                        "background sample output has no writable storage");
                }
                adapter->sample_writes.push_back(*view);
            }
            for (std::size_t channel = 0;
                 channel < channel_count(port.sample_layout); ++channel) {
                Coverage available;
                for (auto const& view : adapter->sample_writes) {
                    if (view.has_channel(channel)) {
                        available.include(view.coverage());
                    }
                }
                if (!contains_coverage(available, *adapter->coverage)) {
                    return std::unexpected("background sample output coverage "
                                           "has no complete route");
                }
            }
        } else if (binding.direction == graph_jit::PortDirection::input) {
            for (auto const& direct : plan.storage.direct_events) {
                if (!storage_selected(direct.storage) ||
                    direct.target_subset >= plan.event_target_subsets.size())
                    continue;
                auto const& target =
                    plan.event_target_subsets[direct.target_subset];
                if (target.port.bundle !=
                        port.configured_port.node_bundle_handle ||
                    target.port.port != port.configured_port.port_index) {
                    continue;
                }
                auto const* view = realization_->event_read(direct.storage);
                if (view == nullptr) {
                    return std::unexpected("background event input has no "
                                           "readable direct storage");
                }
                append_unique_view(adapter->event_reads, *view);
            }
            for (auto const storage_index : binding.storage) {
                if (storage_index >= plan.storage.ports.size()) {
                    return std::unexpected(
                        "background event input storage is out of range");
                }
                auto const& storage = plan.storage.ports[storage_index];
                auto const* view = realization_->event_read(storage_index);
                if (view == nullptr) {
                    return std::unexpected(
                        "background event input has no readable storage");
                }
                for (auto const subset_index : storage.target_subsets) {
                    if (subset_index >= plan.event_target_subsets.size()) {
                        return std::unexpected(
                            "background event target subset is out of range");
                    }
                    auto const& target =
                        plan.event_target_subsets[subset_index];
                    if (target.port.bundle ==
                            port.configured_port.node_bundle_handle &&
                        target.port.port == port.configured_port.port_index) {
                        append_unique_view(adapter->event_reads, *view);
                    }
                }
            }
            Coverage available;
            for (auto const& view : adapter->event_reads) {
                available.include(view.coverage());
            }
            if (!contains_coverage(available, *adapter->coverage)) {
                return std::unexpected(
                    "background event input coverage has no complete route");
            }
        } else {
            for (auto const storage_index : binding.storage) {
                if (storage_index >= plan.storage.ports.size()) {
                    return std::unexpected(
                        "background event output storage is out of range");
                }
                auto const* view = realization_->event_write(storage_index);
                if (view == nullptr) {
                    return std::unexpected(
                        "background event output has no writable storage");
                }
                adapter->event_writes.push_back(*view);
            }
            Coverage available;
            for (auto const& view : adapter->event_writes) {
                available.include(view.coverage());
            }
            if (!contains_coverage(available, *adapter->coverage)) {
                return std::unexpected(
                    "background event output coverage has no complete route");
            }
        }
        adapters.push_back(std::move(adapter));
    }

    std::vector<std::unique_ptr<NodeFrameStorage>> node_storage;
    node_storage.reserve(plan.nodes.size());
    std::vector<std::unique_ptr<BackgroundStorageOperationFrame>>
        operation_frames;
    operation_frames.reserve(plan.nodes.size());
    auto built_nodes = nodes_;
    for (graph_jit::BackgroundNodeIndex node = 0; node < plan.nodes.size();
         ++node) {
        auto storage = std::make_unique<NodeFrameStorage>();
        auto& frame = built_nodes[node];
        auto const& node_plan = plan.nodes[node];
        for (auto const port_index : node_plan.inputs) {
            if (port_index >= plan.ports.size()) {
                return std::unexpected(
                    "background node input port is out of range");
            }
            auto const& port = plan.ports[port_index];
            if (!node_plan.authored_tock_execution || !port.random_access_input)
                continue;
            auto const slot = runtime.port_bindings[port_index];
            if (!slot || *slot >= adapters.size()) {
                return std::unexpected(
                    "background Tock input has no runtime binding");
            }
            auto* adapter = adapters[*slot].get();
            if (port.kind == PortKind::sample) {
                storage->sample_inputs.push_back({
                    .data = adapter,
                    .coverage_value = adapter->coverage,
                    .read_sample = &BindingAdapter::read_sample,
                });
            } else {
                storage->event_inputs.push_back({
                    .data = adapter,
                    .coverage_value = adapter->coverage,
                    .for_each_event = &BindingAdapter::for_each_event,
                });
            }
        }
        for (auto const port_index : node_plan.outputs) {
            if (port_index >= plan.ports.size()) {
                return std::unexpected(
                    "background node output port is out of range");
            }
            auto const& port = plan.ports[port_index];
            if (!node_plan.authored_tock_execution ||
                !port.authored_tock_output)
                continue;
            auto const slot = runtime.port_bindings[port_index];
            if (!slot || *slot >= adapters.size()) {
                return std::unexpected(
                    "background Tock output has no runtime binding");
            }
            auto* adapter = adapters[*slot].get();
            if (port.kind == PortKind::sample) {
                storage->sample_outputs.push_back({
                    .data = adapter,
                    .requested_coverage_value = adapter->coverage,
                    .write_sample = &BindingAdapter::write_sample,
                });
            } else {
                storage->event_outputs.push_back({
                    .data = adapter,
                    .requested_coverage_value = adapter->coverage,
                    .write_event = &BindingAdapter::write_event,
                });
            }
        }

        frame.tock.inputs = storage->sample_inputs;
        frame.tock.outputs = storage->sample_outputs;
        frame.tock.event_inputs = storage->event_inputs;
        frame.tock.event_outputs = storage->event_outputs;
        frame.tock.sample_rate = selection_.sample_rate;
        auto const& placement = runtime.node_operations[node];
        auto operation_frame =
            std::make_unique<BackgroundStorageOperationFrame>(
                *realization_, placement.before, placement.after);
        frame.operation_frame = operation_frame.get();
        frame.prepare_operations =
            &BackgroundStorageOperationFrame::prepare_callback;
        frame.finalize_operations =
            &BackgroundStorageOperationFrame::finalize_callback;
        node_storage.push_back(std::move(storage));
        operation_frames.push_back(std::move(operation_frame));
    }

    for (graph_jit::BackgroundReplayInvocationSlot slot = 0;
         slot < runtime.replay_invocations.size(); ++slot) {
        auto const& replay = runtime.replay_invocations[slot];
        if (replay.node >= built_nodes.size() ||
            runtime.node_replay_invocations[replay.node] != slot) {
            return std::unexpected(
                "background replay invocation has an invalid node slot");
        }
        auto const& node = plan.nodes[replay.node];
        if (!node.replays_tick || replay.maximum_block_size == 0 ||
            replay.maximum_block_size != node.replay_maximum_block_size) {
            return std::unexpected(
                "background replay invocation has an invalid block limit");
        }
        auto& storage = *replay_storage_[slot];
        auto& frame = built_nodes[replay.node];
        if (!storage.regions.empty()) {
            auto const sample_capacity = replay_frame_capacity(storage.regions);
            std::size_t sample_input = 0;
            std::size_t event_input = 0;
            for (auto const binding_slot : replay.input_bindings) {
                auto const& binding = runtime.bindings[binding_slot];
                auto const& port = plan.ports[binding.port];
                auto* adapter = adapters[binding_slot].get();
                if (binding.kind == PortKind::sample) {
                    auto raw = std::make_unique<
                        ReplayFrameStorage::SampleStorage>(
                            port.sample_layout, sample_capacity);
                    storage.sample_inputs[sample_input].storage = raw->binding();
                    storage.sample_input_adapters.push_back(adapter);
                    storage.sample_input_storage.push_back(std::move(raw));
                    ++sample_input;
                } else {
                    auto raw = std::make_unique<
                        ReplayFrameStorage::EventStorage>(
                            replay_event_capacity(
                                port.max_events_per_index, storage.regions));
                    storage.event_inputs[event_input].storage =
                        raw->binding(port.event_type);
                    storage.event_input_adapters.push_back(adapter);
                    storage.event_input_storage.push_back(std::move(raw));
                    ++event_input;
                }
            }
            std::size_t sample_output = 0;
            std::size_t event_output = 0;
            for (auto const binding_slot : replay.output_bindings) {
                auto const& binding = runtime.bindings[binding_slot];
                auto const& port = plan.ports[binding.port];
                auto* adapter = adapters[binding_slot].get();
                if (binding.kind == PortKind::sample) {
                    auto raw = std::make_unique<
                        ReplayFrameStorage::SampleStorage>(
                            port.sample_layout, sample_capacity);
                    storage.sample_outputs[sample_output].storage = raw->binding();
                    storage.sample_output_adapters.push_back(adapter);
                    storage.sample_output_storage.push_back(std::move(raw));
                    ++sample_output;
                } else {
                    auto raw = std::make_unique<
                        ReplayFrameStorage::EventStorage>(
                            replay_event_capacity(
                                port.max_events_per_index, storage.regions));
                    storage.event_outputs[event_output] = {
                        .storage = raw->binding(port.event_type),
                        .overflow_count = &raw->overflow,
                        .source_type = port.event_type,
                        .append_existing = true,
                    };
                    storage.event_output_adapters.push_back(adapter);
                    storage.event_output_storage.push_back(std::move(raw));
                    ++event_output;
                }
            }
            operation_frames[replay.node]->set_leaf_hooks(
                &storage, &prepare_replay, &finalize_replay);
            if (!std::ranges::all_of(
                    storage.sample_inputs,
                    [](ReflectedSampleInputPortBinding const& binding) {
                        return valid_replay_sample_storage(binding.storage);
                    }) ||
                !std::ranges::all_of(
                    storage.sample_outputs,
                    [](ReflectedSampleOutputPortBinding const& binding) {
                        return valid_replay_sample_storage(binding.storage);
                    }) ||
                !std::ranges::all_of(
                    storage.event_inputs,
                    [](ReflectedEventInputPortBinding const& binding) {
                        return valid_replay_event_storage(binding.storage);
                    }) ||
                !std::ranges::all_of(
                    storage.event_outputs,
                    [](ReflectedEventOutputPortBinding const& binding) {
                        return valid_replay_event_storage(binding.storage);
                    })) {
                return std::unexpected(
                    "background replay schedule has unbound raw storage");
            }
        }
        frame.replay.sample_input_bindings = storage.sample_inputs;
        frame.replay.sample_output_bindings = storage.sample_outputs;
        frame.replay.event_input_bindings = storage.event_inputs;
        frame.replay.event_output_bindings = storage.event_outputs;
        frame.replay.sample_rate = selection_.sample_rate;
        frame.replay_regions = storage.regions;
    }

    binding_adapters_ = std::move(adapters);
    node_storage_ = std::move(node_storage);
    operation_frames_ = std::move(operation_frames);
    std::ranges::copy(built_nodes, nodes_.begin());
    call_.nodes = nodes_;
    sealed_ = true;
    return {};
}

std::expected<void, std::string>
BackgroundEvaluationCallFrame::validate_evaluation() const
{
    if (!sealed_) {
        return std::unexpected("background call frame is not sealed");
    }
    if (std::ranges::any_of(
            binding_adapters_, [](auto const& adapter) {
                return adapter->rejected_write;
            })) {
        return std::unexpected(
            "background callback attempted a write outside selected output "
            "storage or exceeded an event bound");
    }
    return realization_->validate_produced_storage();
}

graph_jit::BackgroundEvaluationCall&
BackgroundEvaluationCallFrame::call() noexcept
{
    IV_ASSERT(sealed_, "background call frame is not sealed");
    return call_;
}

graph_jit::BackgroundEvaluationCall const&
BackgroundEvaluationCallFrame::call() const noexcept
{
    IV_ASSERT(sealed_, "background call frame is not sealed");
    return call_;
}

} // namespace iv
