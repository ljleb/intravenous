#pragma once

// The public, execution-independent API for a node's compiled sample/event
// ports. It intentionally owns neither request storage nor result storage:
// those are selected only after the whole query has been planned.

#include <intravenous/node/static_port_access.h>
#include <intravenous/node/traits.h>

#include <cstddef>
#include <algorithm>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>

namespace iv {
    struct AccessRequest {
        SampleIndex begin = 0;
        SampleIndex end = 0;
        std::size_t sample_count = 0;

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return begin <= end && (begin != end || sample_count == 0);
        }

        [[nodiscard]] constexpr bool empty() const noexcept
        {
            return sample_count == 0;
        }
    };

    // A request set is a non-owning, immutable view. The planner is free to
    // retain sparse grids, coalesce dense intervals, or use a custom internal
    // representation before presenting this view to a node callback.
    class AccessRequestSet {
        std::span<AccessRequest const> _requests {};

    public:
        constexpr AccessRequestSet() = default;

        constexpr explicit AccessRequestSet(std::span<AccessRequest const> requests)
            : _requests(requests)
        {}

        [[nodiscard]] constexpr std::span<AccessRequest const> requests() const noexcept
        {
            return _requests;
        }

        [[nodiscard]] constexpr bool empty() const noexcept
        {
            return _requests.empty();
        }
    };

    // Compiled event access is lossless over half-open global intervals. It is
    // intentionally separate from AccessRequest: events are not a sampled
    // scalar signal, so there is no sample_count/downsampling dimension.
    struct EventAccessRequest {
        SampleIndex begin = 0;
        SampleIndex end = 0;

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return begin <= end;
        }

        [[nodiscard]] constexpr bool empty() const noexcept
        {
            return begin == end;
        }
    };

    class EventAccessRequestSet {
        std::span<EventAccessRequest const> _requests {};

    public:
        constexpr EventAccessRequestSet() = default;

        constexpr explicit EventAccessRequestSet(
            std::span<EventAccessRequest const> requests)
            : _requests(requests)
        {}

        [[nodiscard]] constexpr std::span<EventAccessRequest const>
        requests() const noexcept
        {
            return _requests;
        }

        [[nodiscard]] constexpr bool empty() const noexcept
        {
            return _requests.empty();
        }
    };

    struct CompiledSampleExtent {
        SampleIndex begin = 0;
        SampleIndex end = 0;

        [[nodiscard]] constexpr bool contains(SampleIndex index) const noexcept
        {
            return begin <= index && index < end;
        }

        [[nodiscard]] constexpr SampleIndex size() const noexcept
        {
            return end >= begin ? end - begin : 0;
        }
    };

    struct CompiledEventExtent {
        SampleIndex begin = 0;
        SampleIndex end = 0;

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return begin <= end;
        }

        [[nodiscard]] constexpr bool contains(
            SampleIndex request_begin, SampleIndex request_end) const noexcept
        {
            return begin <= request_begin
                && request_begin <= request_end
                && request_end <= end;
        }

        [[nodiscard]] constexpr bool empty() const noexcept
        {
            return begin >= end;
        }
    };

    // Query-local, type-erased read access. A missing callback, an index
    // outside the logical extent, and a disconnected compiled input all read
    // as this port's configured neutral value. This makes reads total without
    // committing to a buffer layout or persistent cache.
    struct CompiledInputPort {
        void const* data = nullptr;
        CompiledSampleExtent (*extent_fn)(void const*) = nullptr;
        Sample (*read_sample_fn)(void const*, SampleIndex, std::size_t) = nullptr;
        Sample neutral_value {};

        [[nodiscard]] constexpr CompiledSampleExtent extent() const noexcept
        {
            return extent_fn ? extent_fn(data) : CompiledSampleExtent {};
        }

        [[nodiscard]] constexpr SampleIndex size() const noexcept
        {
            return extent().size();
        }

        [[nodiscard]] constexpr Sample at(
            SampleIndex index, std::size_t channel = 0) const noexcept
        {
            return read_sample_fn && extent().contains(index)
                ? read_sample_fn(data, index, channel)
                : neutral_value;
        }
    };

    // Query-local, type-erased output access. A planner supplies the request
    // set already accumulated for this output and chooses what receives the
    // writes. An absent writer is a deliberate no-op for an unrequested port.
    struct CompiledOutputPort {
        AccessRequestSet request_set {};
        void* data = nullptr;
        void (*write_sample_fn)(void*, SampleIndex, std::size_t, Sample) = nullptr;

        [[nodiscard]] constexpr AccessRequestSet const& requests() const noexcept
        {
            return request_set;
        }

        constexpr void write(
            SampleIndex index, std::size_t channel, Sample value) const noexcept
        {
            if (write_sample_fn) write_sample_fn(data, index, channel, value);
        }
    };

    // Query-local compiled event input. The execution planner chooses the
    // backing representation and provides a lossless interval lookup. A
    // disconnected input or a request outside the logical extent returns an
    // empty sequence rather than fabricating events.
    struct CompiledEventInputPort {
        void const* data = nullptr;
        CompiledEventExtent (*extent_fn)(void const*) = nullptr;
        std::span<TimedEvent const> (*read_events_fn)(
            void const*, SampleIndex, SampleIndex) = nullptr;

        [[nodiscard]] constexpr CompiledEventExtent extent() const noexcept
        {
            return extent_fn ? extent_fn(data) : CompiledEventExtent {};
        }

        [[nodiscard]] constexpr std::span<TimedEvent const> events(
            SampleIndex begin, SampleIndex end) const noexcept
        {
            if (!read_events_fn || begin >= end) return {};
            auto const logical_extent = extent();
            begin = std::max(begin, logical_extent.begin);
            end = std::min(end, logical_extent.end);
            if (begin >= end) return {};
            return read_events_fn(data, begin, end);
        }
    };

    // Query-local compiled event output. The request set is the globally
    // accumulated union for this output. Writes retain the original global
    // timestamp and event value; an absent sink is a deliberate no-op for an
    // unrequested output.
    struct CompiledEventOutputPort {
        EventAccessRequestSet request_set {};
        void* data = nullptr;
        void (*write_event_fn)(void*, TimedEvent const&) = nullptr;

        [[nodiscard]] constexpr EventAccessRequestSet const& requests() const noexcept
        {
            return request_set;
        }

        constexpr void write(TimedEvent const& event) const noexcept
        {
            if (write_event_fn) write_event_fn(data, event);
        }
    };

    namespace details {
        template<ChannelTypeId Type>
        class StaticCompiledInputPortAccess {
            CompiledInputPort const& _port;

        public:
            constexpr explicit StaticCompiledInputPortAccess(CompiledInputPort const& port)
                : _port(port)
            {}

            [[nodiscard]] constexpr CompiledSampleExtent extent() const noexcept
            {
                return _port.extent();
            }

            [[nodiscard]] constexpr SampleIndex size() const noexcept
            {
                return _port.size();
            }

            [[nodiscard]] constexpr Sample at(SampleIndex index) const noexcept
            requires (Type == ChannelTypeId::mono)
            {
                return _port.at(index);
            }

            template<class Channel>
            [[nodiscard]] constexpr Sample at(Channel, SampleIndex index) const noexcept
            requires (Type != ChannelTypeId::mono)
            {
                return _port.at(index, static_channel_ordinal<Type, Channel>());
            }
        };

        template<ChannelTypeId Type>
        class StaticCompiledOutputPortAccess {
            CompiledOutputPort& _port;

        public:
            constexpr explicit StaticCompiledOutputPortAccess(CompiledOutputPort& port)
                : _port(port)
            {}

            [[nodiscard]] constexpr AccessRequestSet const& requests() const noexcept
            {
                return _port.requests();
            }

            constexpr void write(SampleIndex index, Sample value) const noexcept
            requires (Type == ChannelTypeId::mono)
            {
                _port.write(index, 0, value);
            }

            template<class Channel>
            constexpr void write(Channel, SampleIndex index, Sample value) const noexcept
            requires (Type != ChannelTypeId::mono)
            {
                _port.write(index, static_channel_ordinal<Type, Channel>(), value);
            }
        };

        class StaticCompiledEventInputPortAccess {
            CompiledEventInputPort const& _port;

        public:
            constexpr explicit StaticCompiledEventInputPortAccess(
                CompiledEventInputPort const& port)
                : _port(port)
            {}

            [[nodiscard]] constexpr CompiledEventExtent extent() const noexcept
            {
                return _port.extent();
            }

            [[nodiscard]] constexpr std::span<TimedEvent const> events(
                SampleIndex begin, SampleIndex end) const noexcept
            {
                return _port.events(begin, end);
            }
        };

        class StaticCompiledEventOutputPortAccess {
            CompiledEventOutputPort& _port;

        public:
            constexpr explicit StaticCompiledEventOutputPortAccess(
                CompiledEventOutputPort& port)
                : _port(port)
            {}

            [[nodiscard]] constexpr EventAccessRequestSet const& requests() const noexcept
            {
                return _port.requests();
            }

            constexpr void write(TimedEvent const& event) const noexcept
            {
                _port.write(event);
            }
        };
    }

    // access_block sees only compiled ports. Sample and event ports remain in
    // distinct compact spans because their arbitrary-access semantics differ.
    // The later execution layer supplies contexts with one request view per
    // output for the unbatched callback and fully accumulated request sets for
    // the batched callback.
    template<typename Node>
    struct AccessBlockContext {
        std::span<CompiledInputPort const> inputs {};
        std::span<CompiledOutputPort> outputs {};
        std::span<CompiledEventInputPort const> event_inputs {};
        std::span<CompiledEventOutputPort> event_outputs {};
        std::span<std::byte> compiled_state_storage {};

        using CompiledState = typename NodeCompiledState<Node>::Type;

        template<fixed_string Name>
        [[nodiscard]] constexpr auto input() const
        requires details::has_constexpr_port_configs<Node>
        {
            static_assert(is_compiled(details::static_input_config<Node, Name>()),
                "AccessBlockContext can only access inputs declared compiled");
            if constexpr (details::static_input_port_kind<Node, Name>()
                == PortKind::sample) {
                constexpr auto layout = details::static_input_port_layout<Node, Name>();
                constexpr auto port_index =
                    details::static_compiled_input_port_index<Node, Name>();
                IV_ASSERT(port_index < inputs.size(),
                    "compiled sample input port is absent from access context");
                return details::StaticCompiledInputPortAccess<layout.channel_type>(
                    inputs[port_index]);
            } else {
                constexpr auto port_index =
                    details::static_compiled_event_input_port_index<Node, Name>();
                IV_ASSERT(port_index < event_inputs.size(),
                    "compiled event input port is absent from access context");
                return details::StaticCompiledEventInputPortAccess(
                    event_inputs[port_index]);
            }
        }

        template<fixed_string Name>
        [[nodiscard]] constexpr auto output() const
        requires details::has_constexpr_port_configs<Node>
        {
            static_assert(is_compiled(details::static_output_config<Node, Name>()),
                "AccessBlockContext can only access outputs declared compiled");
            if constexpr (details::static_output_port_kind<Node, Name>()
                == PortKind::sample) {
                constexpr auto layout = details::static_output_port_layout<Node, Name>();
                constexpr auto port_index =
                    details::static_compiled_output_port_index<Node, Name>();
                IV_ASSERT(port_index < outputs.size(),
                    "compiled sample output port is absent from access context");
                return details::StaticCompiledOutputPortAccess<layout.channel_type>(
                    outputs[port_index]);
            } else {
                constexpr auto port_index =
                    details::static_compiled_event_output_port_index<Node, Name>();
                IV_ASSERT(port_index < event_outputs.size(),
                    "compiled event output port is absent from access context");
                return details::StaticCompiledEventOutputPortAccess(
                    event_outputs[port_index]);
            }
        }

        [[nodiscard]] std::add_lvalue_reference_t<CompiledState> compiled_state() const
        requires (!std::is_void_v<CompiledState>)
        {
            void* pointer = compiled_state_storage.data();
            std::size_t space = compiled_state_storage.size();
            void* const aligned = std::align(
                alignof(CompiledState), sizeof(CompiledState), pointer, space);
            IV_ASSERT(aligned != nullptr,
                "compiled state storage does not contain the node CompiledState");
            return *static_cast<CompiledState*>(aligned);
        }
    };

    // Batch access retains the complete, already-unioned request set for all
    // compiled outputs in `batch`. The planner also supplies the individual
    // request views used to normalize a simple access_block() implementation.
    // Nothing here chooses an iteration grid or owns request/sample storage.
    template<typename Node>
    struct AccessBlockBatchContext {
        AccessBlockContext<Node> batch {};
        std::span<AccessBlockContext<Node>> unbatched_accesses {};

        template<fixed_string Name>
        [[nodiscard]] constexpr auto input() const
        requires details::has_constexpr_port_configs<Node>
        {
            return batch.template input<Name>();
        }

        template<fixed_string Name>
        [[nodiscard]] constexpr auto output() const
        requires details::has_constexpr_port_configs<Node>
        {
            return batch.template output<Name>();
        }

        [[nodiscard]] std::add_lvalue_reference_t<
            typename NodeCompiledState<Node>::Type> compiled_state() const
        requires (!std::is_void_v<typename NodeCompiledState<Node>::Type>)
        {
            return batch.compiled_state();
        }
    };

    // Block-access propagation contexts deliberately offer only the terse
    // protocol: ctx.input<"x">(ctx.output<"y">()). They cannot expose
    // realtime ports or node State because planning is purely compiled-data
    // dependency propagation.
    template<typename Node>
    struct PropagateBlockAccessContext {
        using PropagateInputAccess = void (*)(
            void*, std::size_t, AccessRequestSet const&);
        using PropagateEventInputAccess = void (*)(
            void*, std::size_t, EventAccessRequestSet const&);

        // One logical extent per compiled sample input, in the same compact
        // ordinal order used by AccessBlockContext::inputs. The normalized
        // propagation operation uses these extents to synthesize conservative
        // full-input requests when the node provides no propagation callback.
        std::span<CompiledSampleExtent const> input_extents {};
        std::span<CompiledEventExtent const> event_input_extents {};
        std::span<AccessRequestSet const> output_requests {};
        std::span<EventAccessRequestSet const> event_output_requests {};
        void* user_data = nullptr;
        PropagateInputAccess propagate_input_access = nullptr;
        PropagateEventInputAccess propagate_event_input_access = nullptr;

        template<fixed_string Name>
        [[nodiscard]] constexpr AccessRequestSet const& output() const
        requires details::has_constexpr_port_configs<Node>
            && (details::static_output_port_kind<Node, Name>() == PortKind::sample)
        {
            static_assert(details::static_output_port_is_compiled<Node, Name>(),
                "PropagateBlockAccessContext can only inspect compiled outputs");
            constexpr auto port_index = details::static_compiled_output_port_index<Node, Name>();
            IV_ASSERT(port_index < output_requests.size(),
                "compiled output request set is absent from block-access propagation context");
            return output_requests[port_index];
        }

        template<fixed_string Name>
        [[nodiscard]] constexpr EventAccessRequestSet const& output() const
        requires details::has_constexpr_port_configs<Node>
            && (details::static_output_port_kind<Node, Name>() == PortKind::event)
        {
            static_assert(details::static_event_output_port_is_compiled<Node, Name>(),
                "PropagateBlockAccessContext can only inspect compiled outputs");
            constexpr auto port_index =
                details::static_compiled_event_output_port_index<Node, Name>();
            IV_ASSERT(port_index < event_output_requests.size(),
                "compiled event output request set is absent from block-access propagation context");
            return event_output_requests[port_index];
        }

        template<fixed_string Name>
        constexpr void input(AccessRequestSet const& requests) const
        requires details::has_constexpr_port_configs<Node>
            && (details::static_input_port_kind<Node, Name>() == PortKind::sample)
        {
            static_assert(
                details::static_input_port_is_compiled<Node, Name>(),
                "PropagateBlockAccessContext can only propagate access to compiled inputs");
            constexpr auto port_index = details::static_compiled_input_port_index<Node, Name>();
            IV_ASSERT(propagate_input_access != nullptr,
                "block-access propagation context has no compiled-input request sink");
            propagate_input_access(user_data, port_index, requests);
        }

        template<fixed_string Name>
        constexpr void input(EventAccessRequestSet const& requests) const
        requires details::has_constexpr_port_configs<Node>
            && (details::static_input_port_kind<Node, Name>() == PortKind::event)
        {
            static_assert(details::static_event_input_port_is_compiled<Node, Name>(),
                "PropagateBlockAccessContext can only propagate access to compiled inputs");
            constexpr auto port_index =
                details::static_compiled_event_input_port_index<Node, Name>();
            IV_ASSERT(propagate_event_input_access != nullptr,
                "block-access propagation context has no compiled-event-input request sink");
            propagate_event_input_access(user_data, port_index, requests);
        }
    };

    // The batch form mirrors AccessBlockBatchContext: `batch` exposes the
    // accumulated output requests, while `unbatched_propagations` holds the
    // planner-provided individual views for the simple callback form.
    template<typename Node>
    struct PropagateBlockAccessBatchContext {
        PropagateBlockAccessContext<Node> batch {};
        std::span<PropagateBlockAccessContext<Node>> unbatched_propagations {};

        template<fixed_string Name>
        [[nodiscard]] constexpr auto const& output() const
        requires details::has_constexpr_port_configs<Node>
        {
            return batch.template output<Name>();
        }

        template<fixed_string Name, typename Requests>
        constexpr void input(Requests const& requests) const
        requires details::has_constexpr_port_configs<Node>
        {
            batch.template input<Name>(requests);
        }
    };

    template<typename Node>
    void do_access_block_batched(
        Node const& node, AccessBlockBatchContext<Node>& context)
    {
        static_assert(
            details::has_valid_access_block_callback_v<Node>,
            "do_access_block_batched requires exactly one of access_block(...) or access_block_batch(...)");

        if constexpr (details::access_block_callback_kind_v<Node>
            == CompiledPortCallbackKind::batch) {
            node.access_block_batch(context);
        } else {
            for (AccessBlockContext<Node>& request : context.unbatched_accesses) {
                node.access_block(request);
            }
        }
    }

    template<typename Node>
    constexpr PropagateBlockAccessBatchedOperation<Node>
    do_propagate_block_access_batched()
    {
        static_assert(
            details::propagate_block_access_callback_kind_v<Node>
                != CompiledPortCallbackKind::conflicting,
            "do_propagate_block_access_batched cannot normalize both propagate_block_access(...) and propagate_block_access_batch(...)");

        if constexpr (!details::declares_compiled_outputs_v<Node>
            || !details::declares_compiled_inputs_v<Node>) {
            return +[](Node const&, PropagateBlockAccessBatchContext<Node>&) {};
        } else if constexpr (details::propagate_block_access_callback_kind_v<Node>
            == CompiledPortCallbackKind::batch) {
            return +[](Node const& node, PropagateBlockAccessBatchContext<Node>& context) {
                node.propagate_block_access_batch(context);
            };
        } else if constexpr (details::propagate_block_access_callback_kind_v<Node>
            == CompiledPortCallbackKind::unbatched) {
            return +[](Node const& node, PropagateBlockAccessBatchContext<Node>& context) {
                for (PropagateBlockAccessContext<Node>& propagation
                    : context.unbatched_propagations) {
                    node.propagate_block_access(propagation);
                }
            };
        } else {
            return +[](Node const&, PropagateBlockAccessBatchContext<Node>& context) {
                auto const& propagation = context.batch;

                for (std::size_t input_index = 0;
                     input_index < propagation.input_extents.size(); ++input_index) {
                    CompiledSampleExtent const extent =
                        propagation.input_extents[input_index];
                    SampleIndex const sample_count = extent.size();
                    if (sample_count == 0) continue;

                    IV_ASSERT(propagation.propagate_input_access != nullptr,
                        "default block-access propagation has no compiled-input request sink");
                    IV_ASSERT(
                        sample_count <= static_cast<SampleIndex>(
                            std::numeric_limits<std::size_t>::max()),
                        "compiled input extent is too large for an access request");
                    AccessRequest const request {
                        .begin = extent.begin,
                        .end = extent.end,
                        .sample_count = static_cast<std::size_t>(sample_count),
                    };
                    AccessRequestSet const requests {
                        std::span<AccessRequest const>(&request, 1)};
                    propagation.propagate_input_access(
                        propagation.user_data, input_index, requests);
                }

                for (std::size_t input_index = 0;
                     input_index < propagation.event_input_extents.size(); ++input_index) {
                    CompiledEventExtent const extent =
                        propagation.event_input_extents[input_index];
                    if (extent.empty()) continue;

                    IV_ASSERT(propagation.propagate_event_input_access != nullptr,
                        "default block-access propagation has no compiled-event-input request sink");
                    EventAccessRequest const request {
                        .begin = extent.begin,
                        .end = extent.end,
                    };
                    EventAccessRequestSet const requests {
                        std::span<EventAccessRequest const>(&request, 1)};
                    propagation.propagate_event_input_access(
                        propagation.user_data, input_index, requests);
                }
            };
        }
    }
}
