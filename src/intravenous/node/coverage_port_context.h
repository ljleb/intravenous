#pragma once

// Public, execution-independent background sample/event port and callback API.
// The executor owns coverage, requests, result storage, and cache pages; these
// views only expose the storage selected for one node invocation.

#include <intravenous/coverage.h>
#include <intravenous/node/static_port_access.h>
#include <intravenous/node/traits.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace iv {

namespace background_port_details {
    inline Coverage const empty_coverage {};

    inline Coverage const& coverage_or_empty(
        Coverage const* coverage) noexcept
    {
        return coverage ? *coverage : empty_coverage;
    }

    template<typename State>
    std::add_lvalue_reference_t<State> state_from(
        std::span<std::byte> storage)
    requires (!std::is_void_v<State>)
    {
        void* pointer = storage.data();
        std::size_t space = storage.size();
        void* const aligned = std::align(
            alignof(State), sizeof(State), pointer, space);
        IV_ASSERT(aligned != nullptr,
            "background state storage does not contain the node TockState");
        return *static_cast<State*>(aligned);
    }
}

struct RandomAccessSampleInputPort {
    void const* data = nullptr;
    Coverage const* coverage_value = nullptr;
    Sample (*read_sample)(void const*, SampleIndex, std::size_t) = nullptr;

    [[nodiscard]] Coverage const& coverage() const noexcept
    {
        return background_port_details::coverage_or_empty(coverage_value);
    }

    [[nodiscard]] Sample at(
        SampleIndex index, std::size_t channel = 0) const noexcept
    {
        IV_ASSERT(coverage().contains(index),
            "background sample input read lies outside published coverage");
        IV_ASSERT(read_sample != nullptr,
            "background sample input has no readable storage");
        return read_sample(data, index, channel);
    }
};

struct TockSampleOutputPort {
    void* data = nullptr;
    Coverage const* requested_coverage_value = nullptr;
    void (*write_sample)(void*, SampleIndex, std::size_t, Sample) = nullptr;

    [[nodiscard]] Coverage const& requested_coverage() const noexcept
    {
        return background_port_details::coverage_or_empty(requested_coverage_value);
    }

    void write(
        SampleIndex index, std::size_t channel, Sample value) const noexcept
    {
        IV_ASSERT(requested_coverage().contains(index),
            "background sample output write lies outside requested coverage");
        IV_ASSERT(write_sample != nullptr,
            "background sample output has no writable storage");
        write_sample(data, index, channel, value);
    }
};

// Event inputs may be storagely split across any number of cache pages. The
// provider invokes the visitor synchronously in global timestamp order without
// requiring a contiguous query-sized buffer.
struct RandomAccessEventInputPort {
    using VisitEvent = void (*)(void*, TimedEvent const&);
    using ForEachEvent = void (*)(
        void const*, SampleIndex, SampleIndex, void*, VisitEvent);

    void const* data = nullptr;
    Coverage const* coverage_value = nullptr;
    ForEachEvent for_each_event = nullptr;

    [[nodiscard]] Coverage const& coverage() const noexcept
    {
        return background_port_details::coverage_or_empty(coverage_value);
    }

    template<typename Fn>
    void for_each(IndexRegion region, Fn&& fn) const
    {
        IV_ASSERT(region.valid() && coverage().contains(region),
            "background event input read lies outside published coverage");
        if (region.empty()) return;
        IV_ASSERT(for_each_event != nullptr,
            "background event input has no readable segmented storage");
        using Visitor = std::remove_reference_t<Fn>;
        for_each_event(
            data,
            region.begin,
            region.end,
            std::addressof(fn),
            +[](void* opaque, TimedEvent const& event) {
                (*static_cast<Visitor*>(opaque))(event);
            });
    }

    template<typename Fn>
    void for_each(
        SampleIndex begin, SampleIndex end, Fn&& fn) const
    {
        for_each(IndexRegion{begin, end}, std::forward<Fn>(fn));
    }
};

struct TockEventOutputPort {
    void* data = nullptr;
    Coverage const* requested_coverage_value = nullptr;
    void (*write_event)(void*, TimedEvent const&) = nullptr;

    [[nodiscard]] Coverage const& requested_coverage() const noexcept
    {
        return background_port_details::coverage_or_empty(requested_coverage_value);
    }

    void write(TimedEvent const& event) const noexcept
    {
        IV_ASSERT(requested_coverage().contains(event.time),
            "background event output write lies outside requested coverage");
        IV_ASSERT(write_event != nullptr,
            "background event output has no writable storage");
        // The callback contract requires nondecreasing timestamps for each
        // output invocation. The concrete sink may validate that in debug
        // builds without imposing a release-time sorting pass here.
        write_event(data, event);
    }
};

struct InputCoverageChange {
    Coverage const* coverage_value = nullptr;
    Coverage const* changed_value = nullptr;

    [[nodiscard]] Coverage const& coverage() const noexcept
    {
        return background_port_details::coverage_or_empty(coverage_value);
    }

    [[nodiscard]] Coverage const& changed() const noexcept
    {
        return background_port_details::coverage_or_empty(changed_value);
    }
};

struct OutputCoverageChange {
    using PublishCoverage = void (*)(void*, Coverage const&);
    using PublishChange = void (*)(void*, Coverage const&);

    void* data = nullptr;
    Coverage const* previous_coverage_value = nullptr;
    PublishCoverage publish_coverage_value = nullptr;
    PublishChange publish_changed_value = nullptr;

    [[nodiscard]] Coverage const& previous_coverage() const noexcept
    {
        return background_port_details::coverage_or_empty(previous_coverage_value);
    }

    void publish_coverage(Coverage const& coverage) const
    {
        IV_ASSERT(publish_coverage_value != nullptr,
            "background forward context has no output-coverage sink");
        publish_coverage_value(data, coverage);
    }

    void change(Coverage const& changed) const
    {
        if (changed.empty()) return;
        IV_ASSERT(publish_changed_value != nullptr,
            "background forward context has no output-change sink");
        publish_changed_value(data, changed);
    }

    void change(IndexRegion changed) const
    {
        change(Coverage{changed});
    }
};

struct OutputCoverageRequirement {
    Coverage const* required_value = nullptr;

    [[nodiscard]] Coverage const& required() const noexcept
    {
        return background_port_details::coverage_or_empty(required_value);
    }
};

struct InputCoverageRequirement {
    using PublishRequirement = void (*)(void*, Coverage const&);

    void* data = nullptr;
    Coverage const* coverage_value = nullptr;
    PublishRequirement publish_required_value = nullptr;

    [[nodiscard]] Coverage const& coverage() const noexcept
    {
        return background_port_details::coverage_or_empty(coverage_value);
    }

    void require(Coverage const& required) const
    {
        if (required.empty()) return;
        IV_ASSERT(publish_required_value != nullptr,
            "background reverse context has no input-requirement sink");
        publish_required_value(data, required);
    }

    void require(IndexRegion required) const
    {
        require(Coverage{required});
    }
};

namespace details {

template<ChannelTypeId Type>
class StaticBackgroundSampleInputAccess {
    RandomAccessSampleInputPort const& port_;

public:
    explicit StaticBackgroundSampleInputAccess(RandomAccessSampleInputPort const& port)
        : port_(port)
    {}

    [[nodiscard]] Coverage const& coverage() const noexcept
    {
        return port_.coverage();
    }

    [[nodiscard]] Sample at(SampleIndex index) const noexcept
    requires (Type == ChannelTypeId::mono)
    {
        return port_.at(index);
    }

    template<class Channel>
    [[nodiscard]] Sample at(Channel, SampleIndex index) const noexcept
    requires (Type != ChannelTypeId::mono)
    {
        return port_.at(index, static_channel_index<Type, Channel>());
    }
};

template<ChannelTypeId Type>
class StaticBackgroundSampleOutputAccess {
    TockSampleOutputPort& port_;

public:
    explicit StaticBackgroundSampleOutputAccess(TockSampleOutputPort& port)
        : port_(port)
    {}

    [[nodiscard]] Coverage const& requested_coverage() const noexcept
    {
        return port_.requested_coverage();
    }

    void write(SampleIndex index, Sample value) const noexcept
    requires (Type == ChannelTypeId::mono)
    {
        port_.write(index, 0, value);
    }

    template<class Channel>
    void write(Channel, SampleIndex index, Sample value) const noexcept
    requires (Type != ChannelTypeId::mono)
    {
        port_.write(index, static_channel_index<Type, Channel>(), value);
    }
};

class StaticBackgroundEventInputAccess {
    RandomAccessEventInputPort const& port_;

public:
    explicit StaticBackgroundEventInputAccess(RandomAccessEventInputPort const& port)
        : port_(port)
    {}

    [[nodiscard]] Coverage const& coverage() const noexcept
    {
        return port_.coverage();
    }

    template<typename Fn>
    void for_each(IndexRegion region, Fn&& fn) const
    {
        port_.for_each(region, std::forward<Fn>(fn));
    }

    template<typename Fn>
    void for_each(SampleIndex begin, SampleIndex end, Fn&& fn) const
    {
        port_.for_each(begin, end, std::forward<Fn>(fn));
    }
};

class StaticBackgroundEventOutputAccess {
    TockEventOutputPort& port_;

public:
    explicit StaticBackgroundEventOutputAccess(TockEventOutputPort& port)
        : port_(port)
    {}

    [[nodiscard]] Coverage const& requested_coverage() const noexcept
    {
        return port_.requested_coverage();
    }

    void write(TimedEvent const& event) const noexcept { port_.write(event); }
};

template<typename Node, fixed_string Name>
constexpr std::size_t background_input_index()
{
    if constexpr (static_input_port_kind<Node, Name>() == PortKind::sample) {
        return static_random_access_input_port_index<Node, Name>();
    } else {
        return static_random_access_event_input_port_index<Node, Name>();
    }
}

} // namespace details

template<typename Node>
struct TockCoverageContext {
    std::span<RandomAccessSampleInputPort const> inputs {};
    std::span<TockSampleOutputPort> outputs {};
    std::span<RandomAccessEventInputPort const> event_inputs {};
    std::span<TockEventOutputPort> event_outputs {};
    std::span<std::byte> background_state_storage {};
    std::size_t sample_rate = 48000;

    using TockState = typename NodeBackgroundState<Node>::Type;

    template<fixed_string Name>
    [[nodiscard]] auto input() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(is_random_access(details::static_input_config<Node, Name>()),
            "TockCoverageContext can only access inputs declared background");
        if constexpr (details::static_input_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto layout = details::static_input_port_layout<Node, Name>();
            constexpr auto port_index =
                details::static_random_access_input_port_index<Node, Name>();
            IV_ASSERT(port_index < inputs.size(),
                "background sample input is absent from tock context");
            return details::StaticBackgroundSampleInputAccess<layout.channel_type>(
                inputs[port_index]);
        } else {
            constexpr auto port_index =
                details::static_random_access_event_input_port_index<Node, Name>();
            IV_ASSERT(port_index < event_inputs.size(),
                "background event input is absent from tock context");
            return details::StaticBackgroundEventInputAccess(event_inputs[port_index]);
        }
    }

    template<fixed_string Name>
    [[nodiscard]] auto output() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(
            details::static_output_port_is_tock<Node, Name>(),
            "TockCoverageContext can only write background outputs");
        if constexpr (details::static_output_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto layout = details::static_output_port_layout<Node, Name>();
            constexpr auto port_index =
                details::static_tock_output_port_index<Node, Name>();
            IV_ASSERT(port_index < outputs.size(),
                "background sample output is absent from tock context");
            return details::StaticBackgroundSampleOutputAccess<layout.channel_type>(
                outputs[port_index]);
        } else {
            constexpr auto port_index =
                details::static_tock_event_output_port_index<Node, Name>();
            IV_ASSERT(port_index < event_outputs.size(),
                "background event output is absent from tock context");
            return details::StaticBackgroundEventOutputAccess(event_outputs[port_index]);
        }
    }

    [[nodiscard]] std::add_lvalue_reference_t<TockState> tock_state() const
    requires (!std::is_void_v<TockState>)
    {
        return background_port_details::state_from<TockState>(background_state_storage);
    }
};

template<typename Node>
struct PropagateForwardCoverageContext {
    std::span<InputCoverageChange const> inputs {};
    std::span<OutputCoverageChange> outputs {};
    std::span<InputCoverageChange const> event_inputs {};
    std::span<OutputCoverageChange> event_outputs {};
    bool local_state_changed = false;
    std::size_t sample_rate = 48000;

    template<fixed_string Name>
    [[nodiscard]] InputCoverageChange const& input() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(is_random_access(details::static_input_config<Node, Name>()),
            "forward coverage can only inspect background inputs");
        if constexpr (details::static_input_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto index = details::static_random_access_input_port_index<Node, Name>();
            IV_ASSERT(index < inputs.size(),
                "background sample input is absent from forward context");
            return inputs[index];
        } else {
            constexpr auto index =
                details::static_random_access_event_input_port_index<Node, Name>();
            IV_ASSERT(index < event_inputs.size(),
                "background event input is absent from forward context");
            return event_inputs[index];
        }
    }

    template<fixed_string Name>
    [[nodiscard]] OutputCoverageChange const& output() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(
            details::static_output_port_is_tock<Node, Name>(),
            "forward coverage can only publish background outputs");
        if constexpr (details::static_output_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto index = details::static_tock_output_port_index<Node, Name>();
            IV_ASSERT(index < outputs.size(),
                "background sample output is absent from forward context");
            return outputs[index];
        } else {
            constexpr auto index =
                details::static_tock_event_output_port_index<Node, Name>();
            IV_ASSERT(index < event_outputs.size(),
                "background event output is absent from forward context");
            return event_outputs[index];
        }
    }

};

template<typename Node>
struct PropagateReverseCoverageContext {
    std::span<InputCoverageRequirement> inputs {};
    std::span<OutputCoverageRequirement const> outputs {};
    std::span<InputCoverageRequirement> event_inputs {};
    std::span<OutputCoverageRequirement const> event_outputs {};
    std::size_t sample_rate = 48000;

    template<fixed_string Name>
    [[nodiscard]] OutputCoverageRequirement const& output() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(
            details::static_output_port_is_tock<Node, Name>(),
            "reverse coverage can only inspect background outputs");
        if constexpr (details::static_output_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto index = details::static_tock_output_port_index<Node, Name>();
            IV_ASSERT(index < outputs.size(),
                "background sample output is absent from reverse context");
            return outputs[index];
        } else {
            constexpr auto index =
                details::static_tock_event_output_port_index<Node, Name>();
            IV_ASSERT(index < event_outputs.size(),
                "background event output is absent from reverse context");
            return event_outputs[index];
        }
    }

    template<fixed_string Name>
    [[nodiscard]] InputCoverageRequirement const& input() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(is_random_access(details::static_input_config<Node, Name>()),
            "reverse coverage can only require background inputs");
        if constexpr (details::static_input_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto index = details::static_random_access_input_port_index<Node, Name>();
            IV_ASSERT(index < inputs.size(),
                "background sample input is absent from reverse context");
            return inputs[index];
        } else {
            constexpr auto index =
                details::static_random_access_event_input_port_index<Node, Name>();
            IV_ASSERT(index < event_inputs.size(),
                "background event input is absent from reverse context");
            return event_inputs[index];
        }
    }

};

template<typename Node>
void do_tock_coverage(Node const& node, TockCoverageContext<Node>& context)
{
    node.tock_coverage(context);
}

template<typename Node>
void do_propagate_forward_coverage(
    Node const& node, PropagateForwardCoverageContext<Node>& context)
{
    node.propagate_forward_coverage(context);
}

template<typename Node>
void do_propagate_reverse_coverage(
    Node const& node, PropagateReverseCoverageContext<Node>& context)
{
    if constexpr (details::has_propagate_reverse_coverage<Node>) {
        node.propagate_reverse_coverage(context);
    } else {
        bool required = false;
        for (auto const& output : context.outputs) {
            required = required || !output.required().empty();
        }
        for (auto const& output : context.event_outputs) {
            required = required || !output.required().empty();
        }
        if (!required) return;
        for (auto const& input : context.inputs) input.require(input.coverage());
        for (auto const& input : context.event_inputs) input.require(input.coverage());
    }
}

} // namespace iv
