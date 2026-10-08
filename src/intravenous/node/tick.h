#pragma once

#include <intravenous/node/coverage_port_context.h>
#include <intravenous/node/traits.h>
#include <intravenous/node/resources.h>
#include <intravenous/node/static_port_access.h>

#include <array>
#include <memory>
#include <span>
#include <utility>

namespace iv {
    namespace details {
        template<typename Node, bool HasRecording =
            (static_recording_sample_output_count_v<Node> != 0
                || static_recording_event_output_count_v<Node> != 0)>
        struct TickRecordingBindings {};

        template<typename Node>
        struct TickRecordingBindings<Node, true> {
            std::array<
                OutputDisposition*,
                static_recording_sample_output_count_v<Node>> sample_outputs {};
            std::array<
                OutputDisposition*,
                static_recording_event_output_count_v<Node>> event_outputs {};
        };

        template<typename Node>
        struct TickPortBindings {
            std::span<InputPort> sample_inputs {};
            std::span<OutputPort> sample_outputs {};
            std::span<EventInputPort> event_inputs {};
            std::span<EventOutputPort> event_outputs {};
            std::span<RandomAccessSampleInputPort const> random_access_inputs {};
            std::span<RandomAccessEventInputPort const>
                random_access_event_inputs {};
            [[no_unique_address]] TickRecordingBindings<Node> recording {};
        };

        struct TickContextAccess;
    }

    template<typename Node>
    class TickContext {
        details::TickPortBindings<Node> _ports {};

        friend struct details::TickContextAccess;

    protected:
        [[nodiscard]] details::TickPortBindings<Node> const& port_bindings()
            const noexcept
        {
            return _ports;
        }

        template<size_t Index>
        [[nodiscard]] OutputDisposition& recording_disposition() const noexcept
        requires (details::static_output_port_is_recording_at<Node, Index>())
        {
            OutputDisposition* result = nullptr;
            if constexpr (details::static_output_port_kind_at<
                    Node, Index>() == PortKind::sample) {
                constexpr auto recording_index =
                    details::static_recording_sample_output_index_at<
                        Node, Index>();
                result = _ports.recording.sample_outputs[recording_index];
            } else {
                constexpr auto recording_index =
                    details::static_recording_event_output_index_at<
                        Node, Index>();
                result = _ports.recording.event_outputs[recording_index];
            }
            IV_ASSERT(result != nullptr,
                "a Tick/persisted output requires recording disposition");
            return *result;
        }

    public:
        size_t sample_rate = 48000;
        size_t scc_feedback_latency = 0;
        std::span<std::byte> buffer = {};

        TickContext() = default;

        explicit TickContext(
            details::TickPortBindings<Node> ports,
            size_t sample_rate = 48000,
            size_t scc_feedback_latency = 0,
            std::span<std::byte> buffer = {})
            : _ports(std::move(ports))
            , sample_rate(sample_rate)
            , scc_feedback_latency(scc_feedback_latency)
            , buffer(buffer)
        {}

        constexpr double sample_period() const noexcept
        {
            return 1.0 / static_cast<double>(sample_rate);
        }

        using State = typename NodeState<Node>::Type;

        std::add_lvalue_reference_t<State> state() const
        requires(!std::is_void_v<State>);
    };

    namespace details {
        struct TickContextAccess {
            template<typename Node>
            static auto& ports(TickContext<Node> const& context) noexcept
            {
                return context._ports;
            }
        };

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

            [[nodiscard]] auto events(
                SampleIndex begin, std::size_t count) const
            {
                return _port.get_block(
                    static_cast<std::size_t>(begin), count);
            }

            template<typename Fn>
            void for_each(Fn&& fn) const
            {
                _port.for_each_in_block(
                    static_cast<std::size_t>(_index),
                    _block_size,
                    std::forward<Fn>(fn));
            }

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

        template<bool Recording>
        class StaticEventOutputBlockTickAccess
            : private StaticOutputDispositionObserver<Recording> {
            EventOutputPort& _port;
            SampleIndex _index = 0;
            std::size_t _block_size = 0;

        public:
            constexpr StaticEventOutputBlockTickAccess(
                EventOutputPort& port,
                SampleIndex index,
                std::size_t block_size)
            requires (!Recording)
                : StaticOutputDispositionObserver<Recording>()
                , _port(port), _index(index), _block_size(block_size)
            {}

            constexpr StaticEventOutputBlockTickAccess(
                EventOutputPort& port,
                SampleIndex index,
                std::size_t block_size,
                OutputDisposition& disposition)
            requires Recording
                : StaticOutputDispositionObserver<Recording>(disposition)
                , _port(port), _index(index), _block_size(block_size)
            {}

            void push(Event event, std::size_t sample_offset = 0) const
            {
                if constexpr (Recording) this->observe_write();
                _port.push(
                    std::move(event),
                    sample_offset,
                    static_cast<std::size_t>(_index),
                    _block_size);
            }

            void push(TimedEvent const& event) const
            {
                if constexpr (Recording) this->observe_write();
                _port.push(
                    event,
                    static_cast<std::size_t>(_index),
                    _block_size);
            }

            void push_block(BlockView<TimedEvent const> events) const
            {
                if constexpr (Recording) this->observe_write();
                _port.push_block(
                    events,
                    static_cast<std::size_t>(_index),
                    _block_size);
            }

            void write_void() const requires Recording
            {
                this->observe_void();
            }

        };

    }

    template<typename Node>
    struct TickSampleContext : public TickContext<Node> {
        SampleIndex index;

        TickSampleContext(TickContext<Node> base, SampleIndex index);

        template<size_t Index>
        auto input() const
        requires details::has_constexpr_port_configs<Node>
        {
            constexpr auto port_kind =
                details::static_input_port_kind_at<Node, Index>();
            auto const& ports = this->port_bindings();
            if constexpr (port_kind == PortKind::sample) {
                constexpr auto layout =
                    details::static_input_port_layout_at<Node, Index>();
                constexpr auto port_index =
                    details::static_sample_input_port_index_at<Node, Index>();
                IV_ASSERT(port_index < ports.sample_inputs.size(),
                    "static input port is absent from execution context");
                IV_ASSERT(ports.sample_inputs[port_index].channel_layout() == layout,
                    "static input port layout does not match execution context");
                constexpr bool background =
                    details::static_input_port_is_random_access_at<Node, Index>();
                if constexpr (background) {
                    constexpr auto background_index =
                        details::static_random_access_input_port_index_at<
                            Node, Index>();
                    auto const* background_port =
                        background_index < ports.random_access_inputs.size()
                        ? &ports.random_access_inputs[background_index] : nullptr;
                    return details::StaticBackgroundInputSampleTickAccess<layout.channel_type>(
                        ports.sample_inputs[port_index], background_port);
                } else {
                    return details::StaticInputSamplePortAccess<layout.channel_type>(
                        ports.sample_inputs[port_index]);
                }
            } else {
                constexpr auto port_index =
                    details::static_event_input_port_index_at<Node, Index>();
                IV_ASSERT(port_index < ports.event_inputs.size(),
                    "static event input port is absent from execution context");
                constexpr bool background =
                    details::static_input_port_is_random_access_at<Node, Index>();
                if constexpr (background) {
                    constexpr auto background_index =
                        details::static_random_access_event_input_port_index_at<
                            Node, Index>();
                    auto const* background_port =
                        background_index < ports.random_access_event_inputs.size()
                        ? &ports.random_access_event_inputs[background_index] : nullptr;
                    return details::StaticBackgroundEventInputBlockTickAccess(
                        ports.event_inputs[port_index], this->index, 1, background_port);
                } else {
                    return details::StaticEventInputBlockTickAccess(
                        ports.event_inputs[port_index], this->index, 1);
                }
            }
        }

        template<fixed_string Name>
        auto input() const
        requires details::has_constexpr_port_configs<Node>
        {
            return this->template input<
                details::static_input_declaration_index<Node, Name>()>();
        }

        template<size_t Index>
        auto output() const
        requires details::has_constexpr_port_configs<Node>
        {
            constexpr auto port_kind =
                details::static_output_port_kind_at<Node, Index>();
            auto const& ports = this->port_bindings();
            if constexpr (port_kind == PortKind::sample) {
                static_assert(!details::static_output_port_is_tock_at<Node, Index>(),
                    "tick() cannot write a Tock sample output; produce it from tock_coverage()");
                constexpr auto layout =
                    details::static_declared_output_port_layout_at<Node, Index>();
                constexpr auto port_index =
                    details::static_realtime_sample_output_port_index_at<
                        Node, Index>();
                constexpr bool recording =
                    details::static_output_port_is_recording_at<Node, Index>();
                IV_ASSERT(port_index < ports.sample_outputs.size(),
                    "static output port is absent from execution context");
                IV_ASSERT(ports.sample_outputs[port_index].channel_layout() == layout,
                    "static output port layout does not match execution context");
                if constexpr (recording) {
                    return details::StaticOutputSamplePortAccess<
                        layout.channel_type, true>(
                            ports.sample_outputs[port_index],
                            this->template recording_disposition<Index>());
                } else {
                    return details::StaticOutputSamplePortAccess<
                        layout.channel_type, false>(
                            ports.sample_outputs[port_index]);
                }
            } else {
                static_assert(!details::static_output_port_is_tock_at<Node, Index>(),
                    "tick() cannot write a Tock event output; produce it from tock_coverage()");
                constexpr auto port_index =
                    details::static_realtime_event_output_port_index_at<
                        Node, Index>();
                constexpr bool recording =
                    details::static_output_port_is_recording_at<Node, Index>();
                IV_ASSERT(port_index < ports.event_outputs.size(),
                    "static event output port is absent from execution context");
                if constexpr (recording) {
                    return details::StaticEventOutputBlockTickAccess<true>(
                        ports.event_outputs[port_index],
                        this->index,
                        1,
                        this->template recording_disposition<Index>());
                } else {
                    return details::StaticEventOutputBlockTickAccess<false>(
                        ports.event_outputs[port_index], this->index, 1);
                }
            }
        }

        template<fixed_string Name>
        auto output() const
        requires details::has_constexpr_port_configs<Node>
        {
            return this->template output<
                details::static_output_declaration_index<Node, Name>()>();
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

        template<size_t Index>
        auto input() const
        requires details::has_constexpr_port_configs<Node>
        {
            constexpr auto port_kind =
                details::static_input_port_kind_at<Node, Index>();
            auto const& ports = this->port_bindings();
            if constexpr (port_kind == PortKind::sample) {
                constexpr auto layout =
                    details::static_input_port_layout_at<Node, Index>();
                constexpr auto port_index =
                    details::static_sample_input_port_index_at<Node, Index>();
                IV_ASSERT(port_index < ports.sample_inputs.size(),
                    "static input port is absent from execution context");
                IV_ASSERT(ports.sample_inputs[port_index].channel_layout() == layout,
                    "static input port layout does not match execution context");
                constexpr bool background =
                    details::static_input_port_is_random_access_at<Node, Index>();
                if constexpr (background) {
                    constexpr auto background_index =
                        details::static_random_access_input_port_index_at<
                            Node, Index>();
                    auto const* background_port =
                        background_index < ports.random_access_inputs.size()
                        ? &ports.random_access_inputs[background_index] : nullptr;
                    return details::StaticBackgroundInputBlockTickAccess<
                        layout.channel_type, layout.sample_layout>(
                            ports.sample_inputs[port_index],
                            this->block_size,
                            background_port);
                } else {
                    return details::StaticInputBlockPortAccess<
                        layout.channel_type, layout.sample_layout>(
                            ports.sample_inputs[port_index], this->block_size);
                }
            } else {
                constexpr auto port_index =
                    details::static_event_input_port_index_at<Node, Index>();
                IV_ASSERT(port_index < ports.event_inputs.size(),
                    "static event input port is absent from execution context");
                constexpr bool background =
                    details::static_input_port_is_random_access_at<Node, Index>();
                if constexpr (background) {
                    constexpr auto background_index =
                        details::static_random_access_event_input_port_index_at<
                            Node, Index>();
                    auto const* background_port =
                        background_index < ports.random_access_event_inputs.size()
                        ? &ports.random_access_event_inputs[background_index] : nullptr;
                    return details::StaticBackgroundEventInputBlockTickAccess(
                        ports.event_inputs[port_index],
                        this->index,
                        this->block_size,
                        background_port);
                } else {
                    return details::StaticEventInputBlockTickAccess(
                        ports.event_inputs[port_index],
                        this->index,
                        this->block_size);
                }
            }
        }

        template<fixed_string Name>
        auto input() const
        requires details::has_constexpr_port_configs<Node>
        {
            return this->template input<
                details::static_input_declaration_index<Node, Name>()>();
        }

        template<size_t Index>
        auto output() const
        requires details::has_constexpr_port_configs<Node>
        {
            constexpr auto port_kind =
                details::static_output_port_kind_at<Node, Index>();
            auto const& ports = this->port_bindings();
            if constexpr (port_kind == PortKind::sample) {
                constexpr auto layout =
                    details::static_declared_output_port_layout_at<Node, Index>();
                static_assert(
                    !details::static_output_port_is_tock_at<Node, Index>(),
                    "tick_block() cannot write a Tock sample output; produce it from tock_coverage()");
                constexpr auto port_index =
                    details::static_realtime_sample_output_port_index_at<
                        Node, Index>();
                constexpr bool recording =
                    details::static_output_port_is_recording_at<Node, Index>();
                IV_ASSERT(port_index < ports.sample_outputs.size(),
                    "static output port is absent from execution context");
                IV_ASSERT(ports.sample_outputs[port_index].channel_layout() == layout,
                    "static output port layout does not match execution context");
                if constexpr (recording) {
                    return details::StaticOutputBlockPortAccess<
                        layout.channel_type, layout.sample_layout, true>(
                            ports.sample_outputs[port_index],
                            this->block_size,
                            this->template recording_disposition<Index>());
                } else {
                    return details::StaticOutputBlockPortAccess<
                        layout.channel_type, layout.sample_layout, false>(
                            ports.sample_outputs[port_index], this->block_size);
                }
            } else {
                static_assert(
                    !details::static_output_port_is_tock_at<Node, Index>(),
                    "tick_block() cannot write a Tock event output; produce it from tock_coverage()");
                constexpr auto port_index =
                    details::static_realtime_event_output_port_index_at<
                        Node, Index>();
                constexpr bool recording =
                    details::static_output_port_is_recording_at<Node, Index>();
                IV_ASSERT(port_index < ports.event_outputs.size(),
                    "static event output port is absent from execution context");
                if constexpr (recording) {
                    return details::StaticEventOutputBlockTickAccess<true>(
                        ports.event_outputs[port_index],
                        this->index,
                        this->block_size,
                        this->template recording_disposition<Index>());
                } else {
                    return details::StaticEventOutputBlockTickAccess<false>(
                        ports.event_outputs[port_index],
                        this->index,
                        this->block_size);
                }
            }
        }

        template<fixed_string Name>
        auto output() const
        requires details::has_constexpr_port_configs<Node>
        {
            return this->template output<
                details::static_output_declaration_index<Node, Name>()>();
        }
    };

    template<typename Node>
    struct SkipBlockContext : public TickBlockContext<Node> {
        using TickBlockContext<Node>::input;
        using TickBlockContext<Node>::output;

        SkipBlockContext(
            TickContext<Node> base,
            SampleIndex index,
            size_t block_size)
            : TickBlockContext<Node>(base, index, block_size)
        {}
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

    namespace details {
        template<size_t Count, typename Fn, size_t... Index>
        IV_FORCEINLINE constexpr void for_each_static_port_index_impl(
            Fn&& fn, std::index_sequence<Index...>)
        {
            (fn.template operator()<Index>(), ...);
        }

        template<size_t Count, typename Fn>
        IV_FORCEINLINE constexpr void for_each_static_port_index(Fn&& fn)
        {
            for_each_static_port_index_impl<Count>(
                std::forward<Fn>(fn), std::make_index_sequence<Count>{});
        }

        template<typename Node>
        IV_FORCEINLINE void finish_tick_sample_outputs(
            TickContext<Node> const& context, size_t frame_count)
        {
            if constexpr (has_outputs<Node>) {
                auto const& ports = TickContextAccess::ports(context);
                for_each_static_port_index<Node::outputs().size()>(
                    [&]<size_t Index> {
                        if constexpr (static_output_port_kind_at<Node, Index>()
                            == PortKind::sample
                            && !static_output_port_is_tock_at<Node, Index>()) {
                            constexpr auto output_index =
                                static_realtime_sample_output_port_index_at<
                                    Node, Index>();
                            ports.sample_outputs[output_index].finish_direct_write(
                                frame_count);
                        }
                    });
            }
        }

        template<typename Node>
        IV_FORCEINLINE void advance_tick_sample_inputs(
            TickContext<Node> const& context, size_t frame_count)
        {
            if constexpr (has_inputs<Node>) {
                auto const& ports = TickContextAccess::ports(context);
                for_each_static_port_index<Node::inputs().size()>(
                    [&]<size_t Index> {
                        if constexpr (static_input_port_kind_at<Node, Index>()
                            == PortKind::sample) {
                            constexpr auto input_index =
                                static_sample_input_port_index_at<Node, Index>();
                            advance_input(
                                ports.sample_inputs[input_index], frame_count);
                        }
                    });
            }
        }

        template<typename Node>
        IV_FORCEINLINE void author_default_skip_outputs(
            SkipBlockContext<Node> const& context)
        {
            if constexpr (has_outputs<Node>) {
                for_each_static_port_index<Node::outputs().size()>(
                    [&]<size_t Index> {
                        if constexpr (!static_output_port_is_tock_at<Node, Index>()) {
                            if constexpr (static_output_port_kind_at<Node, Index>()
                                == PortKind::sample) {
                                context.template output<Index>().push_silence(
                                    context.block_size);
                            } else {
                                context.template output<Index>().push_block(
                                    BlockView<TimedEvent const>{});
                            }
                        }
                    });
            }
        }
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
            details::finish_tick_sample_outputs<Node>(ctx, 1);
            details::advance_tick_sample_inputs<Node>(ctx, 1);
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
            details::finish_tick_sample_outputs<Node>(ctx, ctx.block_size);
            details::advance_tick_sample_inputs<Node>(ctx, ctx.block_size);
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
            details::author_default_skip_outputs<Node>(ctx);
        }

        details::finish_tick_sample_outputs<Node>(ctx, ctx.block_size);
        details::advance_tick_sample_inputs<Node>(ctx, ctx.block_size);
    }
}
