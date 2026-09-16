#pragma once

#include <intravenous/node/compiled_port_context.h>
#include <intravenous/node/traits.h>
#include <intravenous/node/resources.h>
#include <intravenous/node/static_port_access.h>

#include <memory>
#include <span>

namespace iv {
    template<typename Node>
    struct TickContext {
        std::span<InputPort> inputs = {};
        std::span<OutputPort> outputs = {};
        std::span<EventInputPort> event_inputs = {};
        std::span<EventOutputPort> event_outputs = {};
        // Compact compiled-input views parallel the compact ordinals used by
        // AccessBlockContext. They are additive capabilities for compiled
        // declarations; ordinary current-block access still uses the realtime
        // port spans above.
        std::span<CompiledInputPort const> compiled_inputs = {};
        std::span<CompiledEventInputPort const> compiled_event_inputs = {};
        // Shared persistent state for compiled-side machinery. tick[_block]
        // may mutate it (for example, a recorder appending realtime input) and
        // access_block[_batch] observes the same object later.
        std::span<std::byte> compiled_state_storage = {};
        size_t sample_rate = 48000;
        size_t scc_feedback_latency = 0;
        std::span<std::byte> buffer = {};

        constexpr double sample_period() const noexcept
        {
            return 1.0 / static_cast<double>(sample_rate);
        }

        using State = typename NodeState<Node>::Type;
        using CompiledState = typename NodeCompiledState<Node>::Type;

        std::add_lvalue_reference_t<State> state() const
        requires(!std::is_void_v<State>);

        std::add_lvalue_reference_t<CompiledState> compiled_state() const
        requires(!std::is_void_v<CompiledState>);
    };

    namespace details {
        template<ChannelTypeId Type>
        class StaticCompiledInputSampleTickAccess
            : public StaticInputSamplePortAccess<Type> {
            CompiledInputPort const* _compiled = nullptr;

        public:
            constexpr StaticCompiledInputSampleTickAccess(
                InputPort const& current, CompiledInputPort const* compiled)
                : StaticInputSamplePortAccess<Type>(current)
                , _compiled(compiled)
            {}

            [[nodiscard]] constexpr CompiledSampleExtent extent() const noexcept
            {
                IV_ASSERT(_compiled != nullptr,
                    "compiled sample input has no arbitrary-access binding");
                return _compiled->extent();
            }

            [[nodiscard]] constexpr SampleIndex size() const noexcept
            {
                return extent().size();
            }

            [[nodiscard]] constexpr Sample at(SampleIndex index) const noexcept
            requires (Type == ChannelTypeId::mono)
            {
                IV_ASSERT(_compiled != nullptr,
                    "compiled sample input has no arbitrary-access binding");
                return _compiled->at(index);
            }

            template<class Channel>
            [[nodiscard]] constexpr Sample at(Channel, SampleIndex index) const noexcept
            requires (Type != ChannelTypeId::mono)
            {
                IV_ASSERT(_compiled != nullptr,
                    "compiled sample input has no arbitrary-access binding");
                return _compiled->at(index, static_channel_ordinal<Type, Channel>());
            }
        };

        template<ChannelTypeId Type, SampleStreamLayout Layout>
        class StaticCompiledInputBlockTickAccess
            : public StaticInputBlockPortAccess<Type, Layout> {
            CompiledInputPort const* _compiled = nullptr;

        public:
            constexpr StaticCompiledInputBlockTickAccess(
                InputPort const& current,
                std::size_t block_size,
                CompiledInputPort const* compiled)
                : StaticInputBlockPortAccess<Type, Layout>(current, block_size)
                , _compiled(compiled)
            {}

            [[nodiscard]] constexpr CompiledSampleExtent extent() const noexcept
            {
                IV_ASSERT(_compiled != nullptr,
                    "compiled sample input has no arbitrary-access binding");
                return _compiled->extent();
            }

            [[nodiscard]] constexpr SampleIndex size() const noexcept
            {
                return extent().size();
            }

            [[nodiscard]] constexpr Sample at(SampleIndex index) const noexcept
            requires (Type == ChannelTypeId::mono)
            {
                IV_ASSERT(_compiled != nullptr,
                    "compiled sample input has no arbitrary-access binding");
                return _compiled->at(index);
            }

            template<class Channel>
            [[nodiscard]] constexpr Sample at(Channel, SampleIndex index) const noexcept
            requires (Type != ChannelTypeId::mono)
            {
                IV_ASSERT(_compiled != nullptr,
                    "compiled sample input has no arbitrary-access binding");
                return _compiled->at(index, static_channel_ordinal<Type, Channel>());
            }
        };

        class StaticEventInputBlockTickAccess {
            EventInputPort const& _port;
            SampleIndex _index = 0;
            std::size_t _block_size = 0;

        public:
            constexpr StaticEventInputBlockTickAccess(
                EventInputPort const& port,
                SampleIndex index,
                std::size_t block_size)
                : _port(port), _index(index), _block_size(block_size)
            {}

            [[nodiscard]] auto events() const
            {
                return _port.get_block(
                    static_cast<std::size_t>(_index), _block_size);
            }

            template<typename Fn>
            void for_each(Fn&& fn) const
            {
                _port.for_each_in_block(
                    static_cast<std::size_t>(_index),
                    _block_size,
                    std::forward<Fn>(fn));
            }

            operator EventInputPort const&() const { return _port; }
        };

        class StaticCompiledEventInputBlockTickAccess
            : public StaticEventInputBlockTickAccess {
            CompiledEventInputPort const* _compiled = nullptr;

        public:
            using StaticEventInputBlockTickAccess::events;
            constexpr StaticCompiledEventInputBlockTickAccess(
                EventInputPort const& current,
                SampleIndex index,
                std::size_t block_size,
                CompiledEventInputPort const* compiled)
                : StaticEventInputBlockTickAccess(current, index, block_size)
                , _compiled(compiled)
            {}

            [[nodiscard]] constexpr CompiledEventExtent extent() const noexcept
            {
                IV_ASSERT(_compiled != nullptr,
                    "compiled event input has no arbitrary-access binding");
                return _compiled->extent();
            }

            [[nodiscard]] constexpr std::span<TimedEvent const> events(
                SampleIndex begin, SampleIndex end) const noexcept
            {
                IV_ASSERT(_compiled != nullptr,
                    "compiled event input has no arbitrary-access binding");
                return _compiled->events(begin, end);
            }
        };

        class StaticEventOutputBlockTickAccess {
            EventOutputPort& _port;
            SampleIndex _index = 0;
            std::size_t _block_size = 0;

        public:
            constexpr StaticEventOutputBlockTickAccess(
                EventOutputPort& port,
                SampleIndex index,
                std::size_t block_size)
                : _port(port), _index(index), _block_size(block_size)
            {}

            void push(Event event, std::size_t sample_offset = 0) const
            {
                _port.push(
                    std::move(event),
                    sample_offset,
                    static_cast<std::size_t>(_index),
                    _block_size);
            }

            void push(TimedEvent const& event) const
            {
                _port.push(event);
            }

            operator EventOutputPort&() const { return _port; }
        };
    }

    template<typename Node>
    struct TickSampleContext : public TickContext<Node> {
        SampleIndex index;

        TickSampleContext(TickContext<Node> base, SampleIndex index);

        template<fixed_string Name>
        auto input() const
        requires details::has_constexpr_port_configs<Node>
        {
            constexpr auto port_kind = details::static_input_port_kind<Node, Name>();
            if constexpr (port_kind == PortKind::sample) {
                constexpr auto layout = details::static_input_port_layout<Node, Name>();
                constexpr auto port_index = details::static_input_port_index<Node, Name>();
                IV_ASSERT(port_index < this->inputs.size(), "static input port is absent from execution context");
                IV_ASSERT(this->inputs[port_index].channel_layout() == layout, "static input port layout does not match execution context");
                constexpr bool compiled =
                    details::static_input_port_is_compiled<Node, Name>();
                if constexpr (compiled) {
                    constexpr auto compiled_index =
                        details::static_compiled_input_port_index<Node, Name>();
                    auto const* compiled = compiled_index < this->compiled_inputs.size()
                        ? &this->compiled_inputs[compiled_index] : nullptr;
                    return details::StaticCompiledInputSampleTickAccess<layout.channel_type>(
                        this->inputs[port_index], compiled);
                } else {
                    return details::StaticInputSamplePortAccess<layout.channel_type>(
                        this->inputs[port_index]);
                }
            } else {
                constexpr auto port_index =
                    details::static_event_input_port_index<Node, Name>();
                IV_ASSERT(port_index < this->event_inputs.size(),
                    "static event input port is absent from execution context");
                constexpr bool compiled =
                    details::static_event_input_port_is_compiled<Node, Name>();
                if constexpr (compiled) {
                    constexpr auto compiled_index =
                        details::static_compiled_event_input_port_index<Node, Name>();
                    auto const* compiled =
                        compiled_index < this->compiled_event_inputs.size()
                        ? &this->compiled_event_inputs[compiled_index] : nullptr;
                    return details::StaticCompiledEventInputBlockTickAccess(
                        this->event_inputs[port_index], this->index, 1, compiled);
                } else {
                    return details::StaticEventInputBlockTickAccess(
                        this->event_inputs[port_index], this->index, 1);
                }
            }
        }

        template<fixed_string Name>
        auto output() const
        requires details::has_constexpr_port_configs<Node>
        {
            constexpr auto port_kind = details::static_output_port_kind<Node, Name>();
            if constexpr (port_kind == PortKind::sample) {
                static_assert(!details::static_output_port_is_compiled<Node, Name>(),
                    "tick/tick_block cannot write a compiled sample output; produce it from access_block/access_block_batch");
                constexpr auto layout = details::static_output_port_layout<Node, Name>();
                constexpr auto port_index = details::static_output_port_index<Node, Name>();
                IV_ASSERT(port_index < this->outputs.size(), "static output port is absent from execution context");
                IV_ASSERT(this->outputs[port_index].channel_layout() == layout, "static output port layout does not match execution context");
                return details::StaticOutputSamplePortAccess<layout.channel_type>(this->outputs[port_index]);
            } else {
                static_assert(!details::static_event_output_port_is_compiled<Node, Name>(),
                    "tick/tick_block cannot write a compiled event output; produce it from access_block/access_block_batch");
                constexpr auto port_index =
                    details::static_event_output_port_index<Node, Name>();
                IV_ASSERT(port_index < this->event_outputs.size(),
                    "static event output port is absent from execution context");
                return details::StaticEventOutputBlockTickAccess(
                    this->event_outputs[port_index], this->index, 1);
            }
        }
    };

    template<typename Node>
    struct TickBlockContext : public TickContext<Node> {
        SampleIndex index;
        size_t block_size;

        TickBlockContext(
            TickContext<Node> base,
            SampleIndex index,
            size_t block_size
        );

        template<fixed_string Name>
        auto input() const
        requires details::has_constexpr_port_configs<Node>
        {
            constexpr auto port_kind = details::static_input_port_kind<Node, Name>();
            if constexpr (port_kind == PortKind::sample) {
                constexpr auto layout = details::static_input_port_layout<Node, Name>();
                constexpr auto port_index = details::static_input_port_index<Node, Name>();
                IV_ASSERT(port_index < this->inputs.size(), "static input port is absent from execution context");
                IV_ASSERT(this->inputs[port_index].channel_layout() == layout, "static input port layout does not match execution context");
                constexpr bool compiled =
                    details::static_input_port_is_compiled<Node, Name>();
                if constexpr (compiled) {
                    constexpr auto compiled_index =
                        details::static_compiled_input_port_index<Node, Name>();
                    auto const* compiled = compiled_index < this->compiled_inputs.size()
                        ? &this->compiled_inputs[compiled_index] : nullptr;
                    return details::StaticCompiledInputBlockTickAccess<
                        layout.channel_type, layout.sample_layout>(
                            this->inputs[port_index], this->block_size, compiled);
                } else {
                    return details::StaticInputBlockPortAccess<
                        layout.channel_type, layout.sample_layout>(
                            this->inputs[port_index], this->block_size);
                }
            } else {
                constexpr auto port_index =
                    details::static_event_input_port_index<Node, Name>();
                IV_ASSERT(port_index < this->event_inputs.size(),
                    "static event input port is absent from execution context");
                constexpr bool compiled =
                    details::static_event_input_port_is_compiled<Node, Name>();
                if constexpr (compiled) {
                    constexpr auto compiled_index =
                        details::static_compiled_event_input_port_index<Node, Name>();
                    auto const* compiled =
                        compiled_index < this->compiled_event_inputs.size()
                        ? &this->compiled_event_inputs[compiled_index] : nullptr;
                    return details::StaticCompiledEventInputBlockTickAccess(
                        this->event_inputs[port_index],
                        this->index,
                        this->block_size,
                        compiled);
                } else {
                    return details::StaticEventInputBlockTickAccess(
                        this->event_inputs[port_index],
                        this->index,
                        this->block_size);
                }
            }
        }

        template<fixed_string Name>
        auto output() const
        requires details::has_constexpr_port_configs<Node>
        {
            constexpr auto port_kind = details::static_output_port_kind<Node, Name>();
            if constexpr (port_kind == PortKind::sample) {
                static_assert(!details::static_output_port_is_compiled<Node, Name>(),
                    "tick/tick_block cannot write a compiled sample output; produce it from access_block/access_block_batch");
                constexpr auto layout = details::static_output_port_layout<Node, Name>();
                constexpr auto port_index = details::static_output_port_index<Node, Name>();
                IV_ASSERT(port_index < this->outputs.size(), "static output port is absent from execution context");
                IV_ASSERT(this->outputs[port_index].channel_layout() == layout, "static output port layout does not match execution context");
                return details::StaticOutputBlockPortAccess<
                    layout.channel_type, layout.sample_layout>(
                        this->outputs[port_index], this->block_size);
            } else {
                static_assert(!details::static_event_output_port_is_compiled<Node, Name>(),
                    "tick/tick_block cannot write a compiled event output; produce it from access_block/access_block_batch");
                constexpr auto port_index =
                    details::static_event_output_port_index<Node, Name>();
                IV_ASSERT(port_index < this->event_outputs.size(),
                    "static event output port is absent from execution context");
                return details::StaticEventOutputBlockTickAccess(
                    this->event_outputs[port_index],
                    this->index,
                    this->block_size);
            }
        }
    };

    template<typename Node>
    struct SkipBlockContext : public TickBlockContext<Node> {
        using TickBlockContext<Node>::TickBlockContext;
    };

    namespace details {
        template <typename Node>
        concept has_tick = requires(Node node, TickSampleContext<Node> state)
        {
            node.tick(state);
        };

        template <typename Node>
        concept has_tick_block = requires(Node node, TickBlockContext<Node> state)
        {
            node.tick_block(state);
        };

        template <typename Node>
        concept has_skip_block = requires(Node node, SkipBlockContext<Node> state)
        {
            node.skip_block(state);
        };
    }

    template<typename Node>
    IV_FORCEINLINE std::add_lvalue_reference_t<typename TickContext<Node>::State> TickContext<Node>::state() const
    requires(!std::is_void_v<State>)
    {
        void* ptr = buffer.data();
        size_t space = buffer.size();
        return *reinterpret_cast<State*>(std::align(alignof(State), sizeof(State), ptr, space));
    }

    template<typename Node>
    IV_FORCEINLINE std::add_lvalue_reference_t<typename TickContext<Node>::CompiledState>
    TickContext<Node>::compiled_state() const
    requires(!std::is_void_v<CompiledState>)
    {
        void* pointer = compiled_state_storage.data();
        std::size_t space = compiled_state_storage.size();
        void* const aligned = std::align(
            alignof(CompiledState), sizeof(CompiledState), pointer, space);
        IV_ASSERT(aligned != nullptr,
            "compiled state storage does not contain the node CompiledState");
        return *static_cast<CompiledState*>(aligned);
    }

    template<typename Node>
    IV_FORCEINLINE TickSampleContext<Node>::TickSampleContext(
        TickContext<Node> base, SampleIndex index)
    : TickContext<Node>(base), index(index)
    {}

    template<typename Node>
    IV_FORCEINLINE TickBlockContext<Node>::TickBlockContext(
        TickContext<Node> base,
        SampleIndex index,
        size_t block_size
    )
    : TickContext<Node>(base)
    , index(index)
    , block_size(block_size)
    {}

    template<typename Node>
    void do_tick_block(Node const& node, TickBlockContext<Node> const& state);

    template<typename Node>
    void do_skip_block(Node const& node, SkipBlockContext<Node> const& state);

    std::span<std::byte> remaining_buffer(
        std::span<std::byte> buffer, std::byte* state_base);

    template<typename NestedNode, typename OuterNode>
    IV_FORCEINLINE TickContext<NestedNode> make_nested_tick_context(
        TickContext<OuterNode> const& outer,
        std::byte* nested_state,
        std::span<InputPort> inputs,
        std::span<OutputPort> outputs,
        std::span<EventInputPort> event_inputs = {},
        std::span<EventOutputPort> event_outputs = {}
    )
    {
        return TickContext<NestedNode> {
            .inputs = inputs,
            .outputs = outputs,
            .event_inputs = event_inputs,
            .event_outputs = event_outputs,
            .compiled_inputs = {},
            .compiled_event_inputs = {},
            .sample_rate = outer.sample_rate,
            .scc_feedback_latency = outer.scc_feedback_latency,
            .buffer = remaining_buffer(outer.buffer, nested_state),
        };
    }

    template<typename Node>
    IV_FORCEINLINE void do_tick(Node const& node, TickSampleContext<Node> const& ctx)
    {
        if constexpr (details::has_tick<Node>)
        {
            node.tick(ctx);
            for (auto& output : ctx.outputs) {
                output.finish_direct_write(1);
            }
            advance_inputs(ctx.inputs, 1);
        }
        else if constexpr (details::has_tick_block<Node>)
        {
            do_tick_block(node, {
                static_cast<TickContext<Node> const&>(ctx),
                ctx.index,
                1,
            });
        }
        else
        {
            static_assert(details::has_tick<Node> || details::has_tick_block<Node>, "node must implement tick() or tick_block()");
        }
    }

    template<typename Node>
    IV_FORCEINLINE void do_tick_block(Node const& node, TickBlockContext<Node> const& ctx)
    {
        if (ctx.block_size == 0) {
            return;
        }
        validate_block_size(ctx.block_size);

        if constexpr (details::has_tick_block<Node>)
        {
            node.tick_block(ctx);
            for (auto& output : ctx.outputs) {
                output.finish_direct_write(ctx.block_size);
            }
            advance_inputs(ctx.inputs, ctx.block_size);
        }
        else
        {
            for (size_t i = 0; i < ctx.block_size; ++i) {
                do_tick(node, {
                    static_cast<TickContext<Node> const&>(ctx),
                    ctx.index + i,
                });
            }
        }
    }

    template<typename Node>
    IV_FORCEINLINE void do_skip_block(Node const& node, SkipBlockContext<Node> const& ctx)
    {
        if (ctx.block_size == 0) {
            return;
        }
        validate_block_size(ctx.block_size);

        if constexpr (details::has_skip_block<Node>)
        {
            node.skip_block(ctx);
        }
        else
        {
            for (auto& output : ctx.outputs) {
                output.push_silence(ctx.block_size);
            }
        }

        advance_inputs(ctx.inputs, ctx.block_size);
    }
}
