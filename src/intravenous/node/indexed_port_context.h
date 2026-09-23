#pragma once

// Public, execution-independent indexed sample/event port and callback API.
// The executor owns coverage, requests, result storage, and cache pages; these
// views only expose the bindings selected for one node invocation.

#include <intravenous/indexed_coverage.h>
#include <intravenous/node/static_port_access.h>
#include <intravenous/node/traits.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace iv {

namespace indexed_port_details {
    inline IndexedCoverage const empty_coverage {};

    inline IndexedCoverage const& coverage_or_empty(
        IndexedCoverage const* coverage) noexcept
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
            "indexed state storage does not contain the node IndexedState");
        return *static_cast<State*>(aligned);
    }
}

struct IndexedSampleInputPort {
    void const* data = nullptr;
    IndexedCoverage const* coverage_value = nullptr;
    Sample (*read_sample)(void const*, SampleIndex, std::size_t) = nullptr;

    [[nodiscard]] IndexedCoverage const& coverage() const noexcept
    {
        return indexed_port_details::coverage_or_empty(coverage_value);
    }

    [[nodiscard]] Sample at(
        SampleIndex index, std::size_t channel = 0) const noexcept
    {
        IV_ASSERT(coverage().contains(index),
            "indexed sample input read lies outside published coverage");
        IV_ASSERT(read_sample != nullptr,
            "indexed sample input has no read binding");
        return read_sample(data, index, channel);
    }
};

struct IndexedSampleOutputPort {
    void* data = nullptr;
    IndexedCoverage const* requested_coverage_value = nullptr;
    void (*write_sample)(void*, SampleIndex, std::size_t, Sample) = nullptr;

    [[nodiscard]] IndexedCoverage const& requested_coverage() const noexcept
    {
        return indexed_port_details::coverage_or_empty(requested_coverage_value);
    }

    void write(
        SampleIndex index, std::size_t channel, Sample value) const noexcept
    {
        IV_ASSERT(requested_coverage().contains(index),
            "indexed sample output write lies outside requested coverage");
        IV_ASSERT(write_sample != nullptr,
            "indexed sample output has no write binding");
        write_sample(data, index, channel, value);
    }
};

// Event inputs may be physically split across any number of cache pages. The
// provider invokes the visitor synchronously in global timestamp order without
// requiring a contiguous query-sized buffer.
struct IndexedEventInputPort {
    using VisitEvent = void (*)(void*, TimedEvent const&);
    using ForEachEvent = void (*)(
        void const*, SampleIndex, SampleIndex, void*, VisitEvent);

    void const* data = nullptr;
    IndexedCoverage const* coverage_value = nullptr;
    ForEachEvent for_each_event = nullptr;

    [[nodiscard]] IndexedCoverage const& coverage() const noexcept
    {
        return indexed_port_details::coverage_or_empty(coverage_value);
    }

    template<typename Fn>
    void for_each(IndexedRegion region, Fn&& fn) const
    {
        IV_ASSERT(region.valid() && coverage().contains(region),
            "indexed event input read lies outside published coverage");
        if (region.empty()) return;
        IV_ASSERT(for_each_event != nullptr,
            "indexed event input has no segmented read binding");
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
        for_each(IndexedRegion{begin, end}, std::forward<Fn>(fn));
    }
};

struct IndexedEventOutputPort {
    void* data = nullptr;
    IndexedCoverage const* requested_coverage_value = nullptr;
    void (*write_event)(void*, TimedEvent const&) = nullptr;

    [[nodiscard]] IndexedCoverage const& requested_coverage() const noexcept
    {
        return indexed_port_details::coverage_or_empty(requested_coverage_value);
    }

    void write(TimedEvent const& event) const noexcept
    {
        IV_ASSERT(requested_coverage().contains(event.time),
            "indexed event output write lies outside requested coverage");
        IV_ASSERT(write_event != nullptr,
            "indexed event output has no write binding");
        // The callback contract requires nondecreasing timestamps for each
        // output invocation. The concrete sink may validate that in debug
        // builds without imposing a release-time sorting pass here.
        write_event(data, event);
    }
};

struct IndexedInputChange {
    IndexedCoverage const* coverage_value = nullptr;
    IndexedCoverage const* changed_value = nullptr;

    [[nodiscard]] IndexedCoverage const& coverage() const noexcept
    {
        return indexed_port_details::coverage_or_empty(coverage_value);
    }

    [[nodiscard]] IndexedCoverage const& changed() const noexcept
    {
        return indexed_port_details::coverage_or_empty(changed_value);
    }
};

struct IndexedOutputChange {
    using PublishCoverage = void (*)(void*, IndexedCoverage const&);
    using PublishChange = void (*)(void*, IndexedCoverage const&);

    void* data = nullptr;
    IndexedCoverage const* previous_coverage_value = nullptr;
    PublishCoverage publish_coverage_value = nullptr;
    PublishChange publish_changed_value = nullptr;

    [[nodiscard]] IndexedCoverage const& previous_coverage() const noexcept
    {
        return indexed_port_details::coverage_or_empty(previous_coverage_value);
    }

    void publish_coverage(IndexedCoverage const& coverage) const
    {
        IV_ASSERT(publish_coverage_value != nullptr,
            "indexed forward context has no output-coverage sink");
        publish_coverage_value(data, coverage);
    }

    void change(IndexedCoverage const& changed) const
    {
        if (changed.empty()) return;
        IV_ASSERT(publish_changed_value != nullptr,
            "indexed forward context has no output-change sink");
        publish_changed_value(data, changed);
    }

    void change(IndexedRegion changed) const
    {
        change(IndexedCoverage{changed});
    }
};

struct IndexedOutputRequirement {
    IndexedCoverage const* required_value = nullptr;

    [[nodiscard]] IndexedCoverage const& required() const noexcept
    {
        return indexed_port_details::coverage_or_empty(required_value);
    }
};

struct IndexedInputRequirement {
    using PublishRequirement = void (*)(void*, IndexedCoverage const&);

    void* data = nullptr;
    IndexedCoverage const* coverage_value = nullptr;
    PublishRequirement publish_required_value = nullptr;

    [[nodiscard]] IndexedCoverage const& coverage() const noexcept
    {
        return indexed_port_details::coverage_or_empty(coverage_value);
    }

    void require(IndexedCoverage const& required) const
    {
        if (required.empty()) return;
        IV_ASSERT(publish_required_value != nullptr,
            "indexed reverse context has no input-requirement sink");
        publish_required_value(data, required);
    }

    void require(IndexedRegion required) const
    {
        require(IndexedCoverage{required});
    }
};

namespace details {

template<ChannelTypeId Type>
class StaticIndexedSampleInputAccess {
    IndexedSampleInputPort const& port_;

public:
    explicit StaticIndexedSampleInputAccess(IndexedSampleInputPort const& port)
        : port_(port)
    {}

    [[nodiscard]] IndexedCoverage const& coverage() const noexcept
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
        return port_.at(index, static_channel_ordinal<Type, Channel>());
    }
};

template<ChannelTypeId Type>
class StaticIndexedSampleOutputAccess {
    IndexedSampleOutputPort& port_;

public:
    explicit StaticIndexedSampleOutputAccess(IndexedSampleOutputPort& port)
        : port_(port)
    {}

    [[nodiscard]] IndexedCoverage const& requested_coverage() const noexcept
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
        port_.write(index, static_channel_ordinal<Type, Channel>(), value);
    }
};

class StaticIndexedEventInputAccess {
    IndexedEventInputPort const& port_;

public:
    explicit StaticIndexedEventInputAccess(IndexedEventInputPort const& port)
        : port_(port)
    {}

    [[nodiscard]] IndexedCoverage const& coverage() const noexcept
    {
        return port_.coverage();
    }

    template<typename Fn>
    void for_each(IndexedRegion region, Fn&& fn) const
    {
        port_.for_each(region, std::forward<Fn>(fn));
    }

    template<typename Fn>
    void for_each(SampleIndex begin, SampleIndex end, Fn&& fn) const
    {
        port_.for_each(begin, end, std::forward<Fn>(fn));
    }
};

class StaticIndexedEventOutputAccess {
    IndexedEventOutputPort& port_;

public:
    explicit StaticIndexedEventOutputAccess(IndexedEventOutputPort& port)
        : port_(port)
    {}

    [[nodiscard]] IndexedCoverage const& requested_coverage() const noexcept
    {
        return port_.requested_coverage();
    }

    void write(TimedEvent const& event) const noexcept { port_.write(event); }
};

template<typename Node, fixed_string Name>
constexpr std::size_t indexed_input_ordinal()
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
    std::span<IndexedSampleInputPort const> inputs {};
    std::span<IndexedSampleOutputPort> outputs {};
    std::span<IndexedEventInputPort const> event_inputs {};
    std::span<IndexedEventOutputPort> event_outputs {};
    std::span<std::byte> indexed_state_storage {};
    std::size_t sample_rate = 48000;

    using IndexedState = typename NodeIndexedState<Node>::Type;

    template<fixed_string Name>
    [[nodiscard]] auto input() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(is_random_access(details::static_input_config<Node, Name>()),
            "TockCoverageContext can only access inputs declared indexed");
        if constexpr (details::static_input_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto layout = details::static_input_port_layout<Node, Name>();
            constexpr auto port_index =
                details::static_random_access_input_port_index<Node, Name>();
            IV_ASSERT(port_index < inputs.size(),
                "indexed sample input is absent from tock context");
            return details::StaticIndexedSampleInputAccess<layout.channel_type>(
                inputs[port_index]);
        } else {
            constexpr auto port_index =
                details::static_random_access_event_input_port_index<Node, Name>();
            IV_ASSERT(port_index < event_inputs.size(),
                "indexed event input is absent from tock context");
            return details::StaticIndexedEventInputAccess(event_inputs[port_index]);
        }
    }

    template<fixed_string Name>
    [[nodiscard]] auto output() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(
            details::static_output_port_is_tock<Node, Name>(),
            "TockCoverageContext can only write indexed outputs");
        if constexpr (details::static_output_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto layout = details::static_output_port_layout<Node, Name>();
            constexpr auto port_index =
                details::static_tock_output_port_index<Node, Name>();
            IV_ASSERT(port_index < outputs.size(),
                "indexed sample output is absent from tock context");
            return details::StaticIndexedSampleOutputAccess<layout.channel_type>(
                outputs[port_index]);
        } else {
            constexpr auto port_index =
                details::static_tock_event_output_port_index<Node, Name>();
            IV_ASSERT(port_index < event_outputs.size(),
                "indexed event output is absent from tock context");
            return details::StaticIndexedEventOutputAccess(event_outputs[port_index]);
        }
    }

    [[nodiscard]] std::add_lvalue_reference_t<IndexedState> indexed_state() const
    requires (!std::is_void_v<IndexedState>)
    {
        return indexed_port_details::state_from<IndexedState>(indexed_state_storage);
    }
};

template<typename Node>
struct PropagateForwardCoverageContext {
    std::span<IndexedInputChange const> inputs {};
    std::span<IndexedOutputChange> outputs {};
    std::span<IndexedInputChange const> event_inputs {};
    std::span<IndexedOutputChange> event_outputs {};
    bool local_state_changed = false;
    std::size_t sample_rate = 48000;

    template<fixed_string Name>
    [[nodiscard]] IndexedInputChange const& input() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(is_random_access(details::static_input_config<Node, Name>()),
            "forward coverage can only inspect indexed inputs");
        if constexpr (details::static_input_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto index = details::static_random_access_input_port_index<Node, Name>();
            IV_ASSERT(index < inputs.size(),
                "indexed sample input is absent from forward context");
            return inputs[index];
        } else {
            constexpr auto index =
                details::static_random_access_event_input_port_index<Node, Name>();
            IV_ASSERT(index < event_inputs.size(),
                "indexed event input is absent from forward context");
            return event_inputs[index];
        }
    }

    template<fixed_string Name>
    [[nodiscard]] IndexedOutputChange const& output() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(
            details::static_output_port_is_tock<Node, Name>(),
            "forward coverage can only publish indexed outputs");
        if constexpr (details::static_output_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto index = details::static_tock_output_port_index<Node, Name>();
            IV_ASSERT(index < outputs.size(),
                "indexed sample output is absent from forward context");
            return outputs[index];
        } else {
            constexpr auto index =
                details::static_tock_event_output_port_index<Node, Name>();
            IV_ASSERT(index < event_outputs.size(),
                "indexed event output is absent from forward context");
            return event_outputs[index];
        }
    }

};

template<typename Node>
struct PropagateReverseCoverageContext {
    std::span<IndexedInputRequirement> inputs {};
    std::span<IndexedOutputRequirement const> outputs {};
    std::span<IndexedInputRequirement> event_inputs {};
    std::span<IndexedOutputRequirement const> event_outputs {};
    std::size_t sample_rate = 48000;

    template<fixed_string Name>
    [[nodiscard]] IndexedOutputRequirement const& output() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(
            details::static_output_port_is_tock<Node, Name>(),
            "reverse coverage can only inspect indexed outputs");
        if constexpr (details::static_output_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto index = details::static_tock_output_port_index<Node, Name>();
            IV_ASSERT(index < outputs.size(),
                "indexed sample output is absent from reverse context");
            return outputs[index];
        } else {
            constexpr auto index =
                details::static_tock_event_output_port_index<Node, Name>();
            IV_ASSERT(index < event_outputs.size(),
                "indexed event output is absent from reverse context");
            return event_outputs[index];
        }
    }

    template<fixed_string Name>
    [[nodiscard]] IndexedInputRequirement const& input() const
    requires details::has_constexpr_port_configs<Node>
    {
        static_assert(is_random_access(details::static_input_config<Node, Name>()),
            "reverse coverage can only require indexed inputs");
        if constexpr (details::static_input_port_kind<Node, Name>()
            == PortKind::sample) {
            constexpr auto index = details::static_random_access_input_port_index<Node, Name>();
            IV_ASSERT(index < inputs.size(),
                "indexed sample input is absent from reverse context");
            return inputs[index];
        } else {
            constexpr auto index =
                details::static_random_access_event_input_port_index<Node, Name>();
            IV_ASSERT(index < event_inputs.size(),
                "indexed event input is absent from reverse context");
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
