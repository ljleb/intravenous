#pragma once

#include <intravenous/node/coverage_port_context.h>
#include <intravenous/node/traits.h>
#include <intravenous/node/resources.h>
#include <intravenous/node/static_port_access.h>

#include <memory>
#include <span>

namespace iv {
    template<typename Node>
    struct TickContext {
        std::span<InputPort> inputs = {};
        // Output spans contain only realtime-declared ports, in declaration
        // order within each data kind. Background outputs belong to tock.
        std::span<OutputPort> outputs = {};
        std::span<EventInputPort> event_inputs = {};
        std::span<EventOutputPort> event_outputs = {};
        // Compact background-input views parallel the compact indices used by
        // TockCoverageContext. They are additive capabilities for background
        // declarations; ordinary current-block access still uses the realtime
        // port spans above.
        std::span<RandomAccessSampleInputPort const> random_access_inputs = {};
        std::span<RandomAccessEventInputPort const> random_access_event_inputs = {};
        size_t sample_rate = 48000;
        size_t scc_feedback_latency = 0;
        std::span<std::byte> buffer = {};

        constexpr double sample_period() const noexcept
        {
            return 1.0 / static_cast<double>(sample_rate);
        }

        using State = typename NodeState<Node>::Type;

        std::add_lvalue_reference_t<State> state() const
        requires(!std::is_void_v<State>);
    };

    namespace details {
        template<ChannelTypeId Type>
        class StaticBackgroundInputSampleTickAccess
            : public StaticInputSamplePortAccess<Type> {
            RandomAccessSampleInputPort const* _background = nullptr;

        public:
            StaticBackgroundInputSampleTickAccess(
                InputPort const& current, RandomAccessSampleInputPort const* background)
                : StaticInputSamplePortAccess<Type>(current)
                , _background(background)
            {}

            [[nodiscard]] Coverage const& coverage() const noexcept
            {
                IV_ASSERT(_background != nullptr,
                    "background sample input has no arbitrary-access binding");
                return _background->coverage();
            }

            [[nodiscard]] Sample at(SampleIndex index) const noexcept
            requires (Type == ChannelTypeId::mono)
            {
                IV_ASSERT(_background != nullptr,
                    "background sample input has no arbitrary-access binding");
                return _background->at(index);
            }

            template<class Channel>
            [[nodiscard]] Sample at(Channel, SampleIndex index) const noexcept
            requires (Type != ChannelTypeId::mono)
            {
                IV_ASSERT(_background != nullptr,
                    "background sample input has no arbitrary-access binding");
                return _background->at(index, static_channel_index<Type, Channel>());
            }
        };

        template<ChannelTypeId Type, SampleStreamLayout Layout>
        class StaticBackgroundInputBlockTickAccess
            : public StaticInputBlockPortAccess<Type, Layout> {
            RandomAccessSampleInputPort const* _background = nullptr;

        public:
            StaticBackgroundInputBlockTickAccess(
                InputPort const& current,
                std::size_t block_size,
                RandomAccessSampleInputPort const* background)
                : StaticInputBlockPortAccess<Type, Layout>(current, block_size)
                , _background(background)
            {}

            [[nodiscard]] Coverage const& coverage() const noexcept
            {
                IV_ASSERT(_background != nullptr,
                    "background sample input has no arbitrary-access binding");
                return _background->coverage();
            }

            [[nodiscard]] Sample at(SampleIndex index) const noexcept
            requires (Type == ChannelTypeId::mono)
            {
                IV_ASSERT(_background != nullptr,
                    "background sample input has no arbitrary-access binding");
                return _background->at(index);
            }

            template<class Channel>
            [[nodiscard]] Sample at(Channel, SampleIndex index) const noexcept
            requires (Type != ChannelTypeId::mono)
            {
                IV_ASSERT(_background != nullptr,
                    "background sample input has no arbitrary-access binding");
                return _background->at(index, static_channel_index<Type, Channel>());
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

        class StaticBackgroundEventInputBlockTickAccess
            : public StaticEventInputBlockTickAccess {
            RandomAccessEventInputPort const* _background = nullptr;

        public:
            using StaticEventInputBlockTickAccess::for_each;

            StaticBackgroundEventInputBlockTickAccess(
                EventInputPort const& current,
                SampleIndex index,
                std::size_t block_size,
                RandomAccessEventInputPort const* background)
                : StaticEventInputBlockTickAccess(current, index, block_size)
                , _background(background)
            {}

            [[nodiscard]] Coverage const& coverage() const noexcept
            {
                IV_ASSERT(_background != nullptr,
                    "background event input has no arbitrary-access binding");
                return _background->coverage();
            }

            template<typename Fn>
            void for_each(IndexRegion region, Fn&& fn) const
            {
                IV_ASSERT(_background != nullptr,
                    "background event input has no arbitrary-access binding");
                _background->for_each(region, std::forward<Fn>(fn));
            }

            template<typename Fn>
            void for_each(SampleIndex begin, SampleIndex end, Fn&& fn) const
            {
                IV_ASSERT(_background != nullptr,
                    "background event input has no arbitrary-access binding");
                _background->for_each(begin, end, std::forward<Fn>(fn));
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
                _port.push(
                    event,
                    static_cast<std::size_t>(_index),
                    _block_size);
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
                constexpr bool background =
                    details::static_input_port_is_random_access<Node, Name>();
                if constexpr (background) {
                    constexpr auto background_index =
                        details::static_random_access_input_port_index<Node, Name>();
                    auto const* background_port = background_index < this->random_access_inputs.size()
                        ? &this->random_access_inputs[background_index] : nullptr;
                    return details::StaticBackgroundInputSampleTickAccess<layout.channel_type>(
                        this->inputs[port_index], background_port);
                } else {
                    return details::StaticInputSamplePortAccess<layout.channel_type>(
                        this->inputs[port_index]);
                }
            } else {
                constexpr auto port_index =
                    details::static_event_input_port_index<Node, Name>();
                IV_ASSERT(port_index < this->event_inputs.size(),
                    "static event input port is absent from execution context");
                constexpr bool background =
                    details::static_event_input_port_is_random_access<Node, Name>();
                if constexpr (background) {
                    constexpr auto background_index =
                        details::static_random_access_event_input_port_index<Node, Name>();
                    auto const* background_port =
                        background_index < this->random_access_event_inputs.size()
                        ? &this->random_access_event_inputs[background_index] : nullptr;
                    return details::StaticBackgroundEventInputBlockTickAccess(
                        this->event_inputs[port_index], this->index, 1, background_port);
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
                static_assert(!details::static_output_port_is_tock<Node, Name>(),
                    "tick() cannot write a Tock sample output; produce it from tock_coverage()");
                constexpr auto layout = details::static_output_port_layout<Node, Name>();
                constexpr auto port_index =
                    details::static_realtime_output_port_index<Node, Name>();
                IV_ASSERT(port_index < this->outputs.size(), "static output port is absent from execution context");
                IV_ASSERT(this->outputs[port_index].channel_layout() == layout, "static output port layout does not match execution context");
                return details::StaticOutputSamplePortAccess<layout.channel_type>(this->outputs[port_index]);
            } else {
                static_assert(!details::static_event_output_port_is_tock<Node, Name>(),
                    "tick() cannot write a Tock event output; produce it from tock_coverage()");
                constexpr auto port_index =
                    details::static_realtime_event_output_port_index<Node, Name>();
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
                constexpr bool background =
                    details::static_input_port_is_random_access<Node, Name>();
                if constexpr (background) {
                    constexpr auto background_index =
                        details::static_random_access_input_port_index<Node, Name>();
                    auto const* background_port = background_index < this->random_access_inputs.size()
                        ? &this->random_access_inputs[background_index] : nullptr;
                    return details::StaticBackgroundInputBlockTickAccess<
                        layout.channel_type, layout.sample_layout>(
                            this->inputs[port_index], this->block_size, background_port);
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
                constexpr bool background =
                    details::static_event_input_port_is_random_access<Node, Name>();
                if constexpr (background) {
                    constexpr auto background_index =
                        details::static_random_access_event_input_port_index<Node, Name>();
                    auto const* background_port =
                        background_index < this->random_access_event_inputs.size()
                        ? &this->random_access_event_inputs[background_index] : nullptr;
                    return details::StaticBackgroundEventInputBlockTickAccess(
                        this->event_inputs[port_index],
                        this->index,
                        this->block_size,
                        background_port);
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
                constexpr auto layout = details::static_output_port_layout<Node, Name>();
                static_assert(
                    !details::static_output_port_is_tock<Node, Name>(),
                    "tick_block() cannot write a Tock sample output; produce it from tock_coverage()");
                constexpr auto port_index =
                    details::static_realtime_output_port_index<Node, Name>();
                IV_ASSERT(port_index < this->outputs.size(),
                    "static output port is absent from execution context");
                IV_ASSERT(this->outputs[port_index].channel_layout() == layout,
                    "static output port layout does not match execution context");
                return details::StaticOutputBlockPortAccess<
                    layout.channel_type, layout.sample_layout>(
                        this->outputs[port_index], this->block_size);
            } else {
                static_assert(
                    !details::static_event_output_port_is_tock<Node, Name>(),
                    "tick_block() cannot write a Tock event output; produce it from tock_coverage()");
                constexpr auto port_index =
                    details::static_realtime_event_output_port_index<Node, Name>();
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
        SkipBlockContext(
            TickContext<Node> base,
            SampleIndex index,
            size_t block_size)
            : TickBlockContext<Node>(base, index, block_size)
        {}

        template<fixed_string Name>
        auto output() const
        requires details::has_constexpr_port_configs<Node>
        {
            constexpr auto port_kind =
                details::static_output_port_kind<Node, Name>();
            if constexpr (port_kind == PortKind::sample) {
                static_assert(
                    !details::static_output_port_is_tock<Node, Name>(),
                    "skip_block() cannot write a Tock output");
            } else {
                static_assert(
                    !details::static_event_output_port_is_tock<Node, Name>(),
                    "skip_block() cannot write a Tock output");
            }
            return TickBlockContext<Node>::template output<Name>();
        }
    };

    template<typename Node>
    IV_FORCEINLINE std::add_lvalue_reference_t<typename TickContext<Node>::State> TickContext<Node>::state() const
    requires(!std::is_void_v<State>)
    {
        void* ptr = buffer.data();
        size_t space = buffer.size();
        return *reinterpret_cast<State*>(std::align(alignof(State), sizeof(State), ptr, space));
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
            .random_access_inputs = {},
            .random_access_event_inputs = {},
            .sample_rate = outer.sample_rate,
            .scc_feedback_latency = outer.scc_feedback_latency,
            .buffer = remaining_buffer(outer.buffer, nested_state),
        };
    }

    // Sequential sample callbacks use invocation-local port facades. A tick()
    // callback owns exactly one sample: after it returns, direct output writes
    // are committed and every input cursor advances by one. do_tick_block()
    // generates block execution for tick()-only nodes by repeating that exact
    // transition for each absolute sample index.
    //
    // A native tick_block() callback instead receives facades anchored at the
    // block's first sample for its entire call. Inputs advance only after the
    // callback returns; block accessors address later frames explicitly.
    // Sequential OutputPort::push* calls advance their own authored cursor, while
    // static/direct block writes are committed once at callback return. A
    // well-formed realtime node authors exactly one frame per output per tick(),
    // or block_size frames per output per tick_block(); the runtime deliberately
    // does not add release-time accounting for under/over-production.
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

    // skip_block() follows the same block anchoring/advancement contract. A
    // custom skip_block owns its output semantics; when absent, the runtime
    // generates one block of silence for every sample output. Inputs always
    // advance by block_size after the skip callback/generated block completes.
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
