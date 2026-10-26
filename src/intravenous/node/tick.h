#pragma once

#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/node/coverage_port_context.h>
#include <intravenous/node/traits.h>
#include <intravenous/node/resources.h>
#include <intravenous/node/static_port_access.h>

#include <array>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
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
            std::span<ReflectedContinuousInputPortBinding const>
                continuous_inputs {};
            std::span<ReflectedContinuousOutputPortBinding const>
                continuous_outputs {};
            std::span<EventInputPort> event_inputs {};
            std::span<EventOutputPort> event_outputs {};
            std::span<RandomAccessContinuousInputPort const> random_access_inputs {};
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

        [[nodiscard]] IV_FORCEINLINE
        FractionalGlobalIndexRange::LatticeProjection project_tick_port(
            std::int32_t pace_exponent,
            FractionalGlobalIndex root_index,
            std::size_t root_block_size)
        {
            auto const projected = FractionalGlobalIndexRange{
                .begin = root_index,
                .extent = {.whole = root_block_size},
            }.try_project_onto_lattice(pace_exponent);
            IV_ASSERT(projected.has_value(),
                "continuous port projection exceeds the supported Q64.64 lattice");
            return *projected;
        }

        [[nodiscard]] constexpr FractionalGlobalExtent tick_lattice_step(
            std::int32_t pace_exponent) noexcept
        {
            IV_ASSERT(
                pace_exponent >= FractionalGlobalIndexRange::minimum_pace_exponent
                    && pace_exponent <= FractionalGlobalIndexRange::maximum_pace_exponent,
                "pointwise Tick pace is outside the Q64.64 lattice");
            if (pace_exponent >= 0) {
                if (pace_exponent == 0) {
                    return {.whole = 1};
                }
                return {
                    .fraction = std::uint64_t{1} << (64 - pace_exponent),
                };
            }
            if (pace_exponent == -64) return {};
            return {
                .whole = std::uint64_t{1} << -pace_exponent,
            };
        }

        template<typename Node, std::size_t DeclarationIndex,
                 TickPortAccessKind Access,
                 class Binding>
        IV_FORCEINLINE auto make_continuous_input_access(
            Binding const& binding,
            FractionalGlobalIndex begin,
            std::uint64_t begin_ordinal,
            std::uint64_t sequential_begin_ordinal,
            std::size_t block_size)
        {
            constexpr auto layout =
                static_input_port_layout_at<Node, DeclarationIndex>();
            using Config = std::remove_cvref_t<decltype(port_config_at<
                DeclarationIndex>(
                    std::declval<decltype(Node::inputs()) const&>()))>;
            using Values = typename Config::value_types;
            if constexpr (std::tuple_size_v<Values> == 1) {
                using Value = std::tuple_element_t<0, Values>;
                return StaticContinuousInputAccess<
                    Value,
                    layout.channel_type,
                    layout.sample_layout,
                    Access>{
                        ContinuousInputPort<Value>{
                            binding,
                            begin,
                            begin_ordinal,
                            sequential_begin_ordinal,
                            block_size}};
            } else {
                return StaticVariantContinuousInputAccess<
                    Values,
                    Binding,
                    layout.channel_type,
                    layout.sample_layout,
                    Access>{
                        binding,
                        begin,
                        begin_ordinal,
                        sequential_begin_ordinal,
                        block_size};
            }
        }

        template<typename Node, std::size_t DeclarationIndex,
                 TickPortAccessKind Access, bool Recording, class Binding>
        IV_FORCEINLINE auto make_continuous_output_access(
            Binding const& binding,
            FractionalGlobalIndex begin,
            std::uint64_t begin_ordinal,
            std::uint64_t sequential_begin_ordinal,
            std::size_t block_size,
            OutputDisposition* disposition = nullptr)
        {
            constexpr auto layout =
                static_declared_output_port_layout_at<Node, DeclarationIndex>();
            using Config = std::remove_cvref_t<decltype(port_config_at<
                DeclarationIndex>(
                    std::declval<decltype(Node::outputs()) const&>()))>;
            using Values = typename Config::value_types;
            if constexpr (std::tuple_size_v<Values> == 1) {
                using Value = std::tuple_element_t<0, Values>;
                auto port = ContinuousOutputPort<Value>{
                    binding,
                    begin,
                    begin_ordinal,
                    sequential_begin_ordinal,
                    block_size};
                if constexpr (Recording) {
                    IV_ASSERT(disposition != nullptr,
                        "recording continuous output has no disposition");
                    return StaticContinuousOutputAccess<
                        Value,
                        layout.channel_type,
                        layout.sample_layout,
                        Access,
                        true>{
                            port, *disposition};
                } else {
                    return StaticContinuousOutputAccess<
                        Value,
                        layout.channel_type,
                        layout.sample_layout,
                        Access,
                        false>{
                            port};
                }
            } else {
                if constexpr (Recording) {
                    IV_ASSERT(disposition != nullptr,
                        "recording continuous output has no disposition");
                    return StaticVariantContinuousOutputAccess<
                        Values,
                        Binding,
                        layout.channel_type,
                        layout.sample_layout,
                        Access,
                        true>{
                            binding,
                            begin,
                            begin_ordinal,
                            sequential_begin_ordinal,
                            block_size,
                            *disposition};
                } else {
                    return StaticVariantContinuousOutputAccess<
                        Values,
                        Binding,
                        layout.channel_type,
                        layout.sample_layout,
                        Access,
                        false>{
                            binding,
                            begin,
                            begin_ordinal,
                            sequential_begin_ordinal,
                            block_size};
                }
            }
        }

        // A Random Access input exposes only the published-snapshot view
        // selected for this callback. Current Tick buffers never extend its
        // coverage, so it has no Sequential current-block access.
        template<typename Node, std::size_t DeclarationIndex>
        [[nodiscard]] constexpr auto make_random_access_continuous_input(
            RandomAccessContinuousInputPort const& port) noexcept
        {
            constexpr auto access =
                static_input_port_access_at<Node, DeclarationIndex>();
            using Config = std::remove_cvref_t<decltype(port_config_at<
                DeclarationIndex>(
                    std::declval<decltype(Node::inputs()) const&>()))>;
            using Values = typename Config::value_types;
            if constexpr (std::tuple_size_v<Values> == 1) {
                using Value = std::tuple_element_t<0, Values>;
                return StaticBackgroundContinuousInputAccess<Value, access>{port};
            } else {
                return StaticVariantBackgroundContinuousInputAccess<
                    Values, access>{port};
            }
        }

        template<TickPortAccessKind Access>
        class StaticEventInputTickAccess {
            EventInputPort const& _port;
            GlobalIndex _index{};
            std::size_t _block_size = 0;

        public:
            constexpr StaticEventInputTickAccess(
                EventInputPort const& port,
                GlobalIndex index,
                std::size_t block_size)
                : _port(port), _index(index), _block_size(block_size)
            {}

            [[nodiscard]] auto events() const
            {
                return _port.get_block(
                    _index, _block_size);
            }

            [[nodiscard]] auto events(
                GlobalIndex begin, std::size_t count) const
            requires (Access == TickPortAccessKind::block)
            {
                return _port.get_block(
                    begin, count);
            }

            template<typename Fn>
            void for_each(Fn&& fn) const
            {
                _port.for_each_in_block(
                    _index,
                    _block_size,
                    std::forward<Fn>(fn));
            }

        };

        template<TickPortAccessKind Access, bool Recording>
        class StaticEventOutputTickAccess
            : private StaticOutputDispositionObserver<Recording> {
            EventOutputPort& _port;
            GlobalIndex _index{};
            std::size_t _block_size = 0;

        public:
            constexpr StaticEventOutputTickAccess(
                EventOutputPort& port,
                GlobalIndex index,
                std::size_t block_size)
            requires (!Recording)
                : StaticOutputDispositionObserver<Recording>()
                , _port(port), _index(index), _block_size(block_size)
            {}

            constexpr StaticEventOutputTickAccess(
                EventOutputPort& port,
                GlobalIndex index,
                std::size_t block_size,
                OutputDisposition& disposition)
            requires Recording
                : StaticOutputDispositionObserver<Recording>(disposition)
                , _port(port), _index(index), _block_size(block_size)
            {}

            void push(Event event) const
            {
                if constexpr (Recording) this->observe_write();
                _port.push(std::move(event), 0, _index, _block_size);
            }

            void push(Event event, std::size_t sample_offset) const
            requires (Access == TickPortAccessKind::block)
            {
                if constexpr (Recording) this->observe_write();
                _port.push(
                    std::move(event),
                    sample_offset,
                    _index,
                    _block_size);
            }

            void push(TimedEvent const& event) const
            {
                if constexpr (Recording) this->observe_write();
                _port.push(
                    event,
                    _index,
                    _block_size);
            }

            void push_block(BlockView<TimedEvent const> events) const
            requires (Access == TickPortAccessKind::block)
            {
                if constexpr (Recording) this->observe_write();
                _port.push_block(
                    events,
                    _index,
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
        FractionalGlobalIndex index;
        std::uint64_t value_index = 0;

    private:
        // This offset selects one value inside each port binding's own
        // compiler-owned Sequential domain. It is not a second node-visible
        // coordinate.
        std::size_t sequential_value_offset_ = 0;

    public:

        TickSampleContext(
            TickContext<Node> base,
            FractionalGlobalIndex index,
            std::uint64_t value_index,
            std::size_t sequential_value_offset);

        TickSampleContext(TickContext<Node> base, GlobalIndex index)
            : TickSampleContext(
                base,
                FractionalGlobalIndex::from_global(index),
                index.bits(),
                0)
        {}

        [[nodiscard]] GlobalIndex event_index() const noexcept
        {
            IV_ASSERT(index.fraction() == 0,
                "event access from pointwise tick() requires project-rate pace");
            return index.whole();
        }

        template<size_t Index>
        auto input() const
        requires details::has_constexpr_port_configs<Node>
        {
            constexpr auto port_kind =
                details::static_input_port_kind_at<Node, Index>();
            auto const& ports = this->port_bindings();
            if constexpr (port_kind == PortKind::sample) {
                if constexpr (details::static_input_port_is_random_access_at<
                                  Node, Index>()) {
                    constexpr auto background_index =
                        details::static_random_access_input_port_index_at<
                            Node, Index>();
                    IV_ASSERT(background_index < ports.random_access_inputs.size(),
                        "static Random Access input port is absent from execution context");
                    return details::make_random_access_continuous_input<
                        Node, Index>(ports.random_access_inputs[background_index]);
                } else {
                    constexpr auto port_index =
                        details::static_sample_input_port_index_at<Node, Index>();
                    IV_ASSERT(port_index < ports.continuous_inputs.size(),
                        "static input port is absent from execution context");
                    auto const& binding = ports.continuous_inputs[port_index];
                    return details::make_continuous_input_access<
                        Node, Index, details::TickPortAccessKind::point>(
                        binding,
                        this->index,
                        this->value_index,
                        binding.sequential_begin_ordinal
                            + this->sequential_value_offset_,
                        1);
                }
            } else {
                if constexpr (details::static_input_port_is_random_access_at<
                                  Node, Index>()) {
                    constexpr auto background_index =
                        details::static_random_access_event_input_port_index_at<
                            Node, Index>();
                    IV_ASSERT(
                        background_index < ports.random_access_event_inputs.size(),
                        "static Random Access event input port is absent from execution context");
                    constexpr auto access =
                        details::static_input_port_access_at<Node, Index>();
                    return details::StaticBackgroundEventInputAccess<access>{
                        ports.random_access_event_inputs[background_index]};
                } else {
                    constexpr auto port_index =
                        details::static_event_input_port_index_at<Node, Index>();
                    IV_ASSERT(port_index < ports.event_inputs.size(),
                        "static event input port is absent from execution context");
                    return details::StaticEventInputTickAccess<
                        details::TickPortAccessKind::point>(
                        ports.event_inputs[port_index], this->event_index(), 1);
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
                IV_ASSERT(port_index < ports.continuous_outputs.size(),
                    "static output port is absent from execution context");
                auto const& binding = ports.continuous_outputs[port_index];
                IV_ASSERT(binding.storage.channel_layout == layout,
                    "static output port layout does not match execution context");
                if constexpr (recording) {
                    return details::make_continuous_output_access<
                        Node,
                        Index,
                        details::TickPortAccessKind::point,
                        true>(
                            binding,
                            this->index,
                            this->value_index,
                            binding.sequential_begin_ordinal
                                + this->sequential_value_offset_,
                            1,
                            &this->template recording_disposition<Index>());
                } else {
                    return details::make_continuous_output_access<
                        Node,
                        Index,
                        details::TickPortAccessKind::point,
                        false>(
                            binding,
                            this->index,
                            this->value_index,
                            binding.sequential_begin_ordinal
                                + this->sequential_value_offset_,
                            1);
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
                    return details::StaticEventOutputTickAccess<
                        details::TickPortAccessKind::point, true>(
                        ports.event_outputs[port_index],
                        this->event_index(),
                        1,
                        this->template recording_disposition<Index>());
                } else {
                    return details::StaticEventOutputTickAccess<
                        details::TickPortAccessKind::point, false>(
                        ports.event_outputs[port_index], this->event_index(), 1);
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
        FractionalGlobalIndex index;
        size_t block_size;

        TickBlockContext(
            TickContext<Node> base,
            FractionalGlobalIndex index,
            size_t block_size
        );

        [[nodiscard]] GlobalIndex event_index() const noexcept
        {
            IV_ASSERT(index.fraction() == 0,
                "event access from tick_block() requires a project-sample boundary");
            return index.whole();
        }

        template<size_t Index>
        auto input() const
        requires details::has_constexpr_port_configs<Node>
        {
            constexpr auto port_kind =
                details::static_input_port_kind_at<Node, Index>();
            auto const& ports = this->port_bindings();
            if constexpr (port_kind == PortKind::sample) {
                if constexpr (details::static_input_port_is_random_access_at<
                                  Node, Index>()) {
                    constexpr auto background_index =
                        details::static_random_access_input_port_index_at<
                            Node, Index>();
                    IV_ASSERT(background_index < ports.random_access_inputs.size(),
                        "static Random Access input port is absent from execution context");
                    return details::make_random_access_continuous_input<
                        Node, Index>(ports.random_access_inputs[background_index]);
                } else {
                    constexpr auto port_index =
                        details::static_sample_input_port_index_at<Node, Index>();
                    IV_ASSERT(port_index < ports.continuous_inputs.size(),
                        "static input port is absent from execution context");
                    auto const& binding = ports.continuous_inputs[port_index];
                    auto const projected = details::project_tick_port(
                        binding.absolute_pace_exponent,
                        this->index,
                        this->block_size);
                    return details::make_continuous_input_access<
                        Node, Index, details::TickPortAccessKind::block>(
                        binding,
                        projected.begin,
                        projected.begin_ordinal_bits,
                        binding.sequential_begin_ordinal,
                        projected.count);
                }
            } else {
                if constexpr (details::static_input_port_is_random_access_at<
                                  Node, Index>()) {
                    constexpr auto background_index =
                        details::static_random_access_event_input_port_index_at<
                            Node, Index>();
                    IV_ASSERT(
                        background_index < ports.random_access_event_inputs.size(),
                        "static Random Access event input port is absent from execution context");
                    constexpr auto access =
                        details::static_input_port_access_at<Node, Index>();
                    return details::StaticBackgroundEventInputAccess<access>{
                        ports.random_access_event_inputs[background_index]};
                } else {
                    constexpr auto port_index =
                        details::static_event_input_port_index_at<Node, Index>();
                    IV_ASSERT(port_index < ports.event_inputs.size(),
                        "static event input port is absent from execution context");
                    return details::StaticEventInputTickAccess<
                        details::TickPortAccessKind::block>(
                        ports.event_inputs[port_index],
                        this->event_index(),
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
                IV_ASSERT(port_index < ports.continuous_outputs.size(),
                    "static output port is absent from execution context");
                auto const& binding = ports.continuous_outputs[port_index];
                IV_ASSERT(binding.storage.channel_layout == layout,
                    "static output port layout does not match execution context");
                auto const projected = details::project_tick_port(
                    binding.absolute_pace_exponent,
                    this->index,
                    this->block_size);
                if constexpr (recording) {
                    return details::make_continuous_output_access<
                        Node,
                        Index,
                        details::TickPortAccessKind::block,
                        true>(
                            binding,
                            projected.begin,
                            projected.begin_ordinal_bits,
                            binding.sequential_begin_ordinal,
                            projected.count,
                            &this->template recording_disposition<Index>());
                } else {
                    return details::make_continuous_output_access<
                        Node,
                        Index,
                        details::TickPortAccessKind::block,
                        false>(
                            binding,
                            projected.begin,
                            projected.begin_ordinal_bits,
                            binding.sequential_begin_ordinal,
                            projected.count);
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
                    return details::StaticEventOutputTickAccess<
                        details::TickPortAccessKind::block, true>(
                        ports.event_outputs[port_index],
                        this->event_index(),
                        this->block_size,
                        this->template recording_disposition<Index>());
                } else {
                    return details::StaticEventOutputTickAccess<
                        details::TickPortAccessKind::block, false>(
                        ports.event_outputs[port_index],
                        this->event_index(),
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
            FractionalGlobalIndex index,
            size_t block_size)
            : TickBlockContext<Node>(base, index, block_size)
        {}
    };

    namespace details {
        template<typename Node, typename Context>
        struct TickBatchLaneBinding {
            Node const* node = nullptr;
            Context const* context = nullptr;
        };

        template<typename Node, typename Context>
        class TickBatchLane {
            TickBatchLaneBinding<Node, Context> const* binding_ = nullptr;

        public:
            TickBatchLane() = default;
            explicit TickBatchLane(
                TickBatchLaneBinding<Node, Context> const& binding) noexcept
                : binding_(std::addressof(binding))
            {}

            [[nodiscard]] Node const& node() const noexcept
            {
                IV_ASSERT(binding_ != nullptr && binding_->node != nullptr,
                    "Tick batch lane has no configured node");
                return *binding_->node;
            }

            [[nodiscard]] Context const& context() const noexcept
            {
                IV_ASSERT(binding_ != nullptr && binding_->context != nullptr,
                    "Tick batch lane has no scalar context");
                return *binding_->context;
            }

            template<std::size_t Index>
            [[nodiscard]] auto input() const
            {
                return context().template input<Index>();
            }

            template<fixed_string Name>
            [[nodiscard]] auto input() const
            {
                return context().template input<Name>();
            }

            template<std::size_t Index>
            [[nodiscard]] auto output() const
            {
                return context().template output<Index>();
            }

            template<fixed_string Name>
            [[nodiscard]] auto output() const
            {
                return context().template output<Name>();
            }

            [[nodiscard]] decltype(auto) state() const
            requires (!std::is_void_v<typename NodeState<Node>::Type>)
            {
                return context().state();
            }
        };

        template<typename Node, typename Context>
        class TickBatchIterator {
            TickBatchLaneBinding<Node, Context> const* current_ = nullptr;

        public:
            using value_type = TickBatchLane<Node, Context>;
            using difference_type = std::ptrdiff_t;
            using iterator_category = std::forward_iterator_tag;

            TickBatchIterator() = default;
            explicit TickBatchIterator(
                TickBatchLaneBinding<Node, Context> const* current) noexcept
                : current_(current)
            {}

            [[nodiscard]] value_type operator*() const noexcept
            {
                return value_type{*current_};
            }

            TickBatchIterator& operator++() noexcept
            {
                ++current_;
                return *this;
            }

            TickBatchIterator operator++(int) noexcept
            {
                auto result = *this;
                ++*this;
                return result;
            }

            friend bool operator==(
                TickBatchIterator const&, TickBatchIterator const&) = default;
        };

        template<typename Node, typename Context>
        class TickBatchContextBase {
            std::span<TickBatchLaneBinding<Node, Context> const> lanes_ {};

        protected:
            explicit TickBatchContextBase(
                std::span<TickBatchLaneBinding<Node, Context> const> lanes) noexcept
                : lanes_(lanes)
            {}

        public:
            using Lane = TickBatchLane<Node, Context>;
            using iterator = TickBatchIterator<Node, Context>;

            [[nodiscard]] std::size_t size() const noexcept
            {
                return lanes_.size();
            }

            [[nodiscard]] bool empty() const noexcept
            {
                return lanes_.empty();
            }

            [[nodiscard]] Lane operator[](std::size_t index) const noexcept
            {
                IV_ASSERT(index < lanes_.size(), "Tick batch lane is out of range");
                return Lane{lanes_[index]};
            }

            [[nodiscard]] iterator begin() const noexcept
            {
                return iterator{lanes_.data()};
            }

            [[nodiscard]] iterator end() const noexcept
            {
                return iterator{lanes_.empty()
                    ? lanes_.data()
                    : lanes_.data() + lanes_.size()};
            }
        };
    }

    template<typename Node>
    class TickBlockBatchContext
        : public details::TickBatchContextBase<Node, TickBlockContext<Node>> {
        using Base = details::TickBatchContextBase<Node, TickBlockContext<Node>>;

    public:
        explicit TickBlockBatchContext(
            std::span<details::TickBatchLaneBinding<
                Node, TickBlockContext<Node>> const> lanes) noexcept
            : Base(lanes)
        {}
    };

    template<typename Node>
    class SkipBlockBatchContext
        : public details::TickBatchContextBase<Node, SkipBlockContext<Node>> {
        using Base = details::TickBatchContextBase<Node, SkipBlockContext<Node>>;

    public:
        explicit SkipBlockBatchContext(
            std::span<details::TickBatchLaneBinding<
                Node, SkipBlockContext<Node>> const> lanes) noexcept
            : Base(lanes)
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
        TickContext<Node> base,
        FractionalGlobalIndex index,
        std::uint64_t value_index,
        std::size_t sequential_value_offset)
    : TickContext<Node>(base)
    , index(index)
    , value_index(value_index)
    , sequential_value_offset_(sequential_value_offset)
    {}

    template<typename Node>
    IV_FORCEINLINE TickBlockContext<Node>::TickBlockContext(
        TickContext<Node> base,
        FractionalGlobalIndex index,
        size_t block_size
    )
    : TickContext<Node>(base)
    , index(index)
    , block_size(block_size)
    {}

    template<typename Node>
    void do_tick_block(Node const& node, TickBlockContext<Node> const& state);

    template<typename Node>
    void do_tick_block_batch(TickBlockBatchContext<Node> const& state);

    template<typename Node>
    void do_skip_block(Node const& node, SkipBlockContext<Node> const& state);

    template<typename Node>
    void do_skip_block_batch(SkipBlockBatchContext<Node> const& state);

    namespace details {
        template<typename Node>
        [[nodiscard]] IV_FORCEINLINE std::int32_t pointwise_tick_pace(
            TickContext<Node> const& context)
        {
            auto const& ports = TickContextAccess::ports(context);
            std::optional<std::int32_t> pace;
            auto accept = [&](std::int32_t candidate) {
                IV_ASSERT(!pace || *pace == candidate,
                    "pointwise tick() received incompatible port paces");
                pace = candidate;
            };
            for (auto const& input : ports.continuous_inputs) {
                accept(input.absolute_pace_exponent);
            }
            for (auto const& output : ports.continuous_outputs) {
                accept(output.absolute_pace_exponent);
            }
            if (!ports.event_inputs.empty() || !ports.event_outputs.empty()) {
                accept(0);
            }
            return pace.value_or(0);
        }

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
        IV_FORCEINLINE void author_default_skip_outputs(
            SkipBlockContext<Node> const& context)
        {
            if constexpr (has_outputs<Node>) {
                for_each_static_port_index<
                    port_config_count_v<decltype(Node::outputs())>>(
                    [&]<size_t Index> {
                        if constexpr (!static_output_port_is_tock_at<Node, Index>()) {
                            if constexpr (static_output_port_kind_at<Node, Index>()
                                == PortKind::sample) {
                                auto output = context.template output<Index>();
                                using Config = std::remove_cvref_t<decltype(
                                    port_config_at<Index>(
                                        std::declval<decltype(Node::outputs()) const&>()))>;
                                using Values = typename Config::value_types;
                                if constexpr (std::tuple_size_v<Values> == 1) {
                                    output.clear_block();
                                } else {
                                    [&]<std::size_t... TypeIndex>(
                                        std::index_sequence<TypeIndex...>) {
                                        (([&] {
                                            using Value = std::tuple_element_t<
                                                TypeIndex, Values>;
                                            if (output.template is<Value>()) {
                                                output.template as<Value>().clear_block();
                                            }
                                        }()), ...);
                                    }(std::make_index_sequence<
                                        std::tuple_size_v<Values>>{});
                                }
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
    // Sequential continuous-output push calls advance the retained facade's
    // authored cursor, while indexed block writes are committed once at callback
    // return. A well-formed realtime node authors exactly one value per output per
    // tick(), or each output's projected block_size() values per tick_block(); the
    // runtime deliberately does not add release-time accounting for
    // under/over-production.
    template<typename Node>
    IV_FORCEINLINE void do_tick(Node const& node, TickSampleContext<Node> const& ctx)
    {
        static_assert(details::has_tick<Node>,
            "do_tick() requires an authored pointwise tick() callback");
        node.tick(ctx);
    }

    template<typename Node>
    IV_FORCEINLINE void do_tick_block(Node const& node, TickBlockContext<Node> const& ctx)
    {
        if (ctx.block_size == 0) {
            return;
        }

        if constexpr (details::has_tick_block_batch<Node>)
        {
            details::TickBatchLaneBinding<Node, TickBlockContext<Node>> lane{
                .node = std::addressof(node),
                .context = std::addressof(ctx),
            };
            TickBlockBatchContext<Node> batch{{std::addressof(lane), 1}};
            Node::tick_block_batch(batch);
        }
        else if constexpr (details::has_tick_block<Node>)
        {
            node.tick_block(ctx);
        }
        else
        {
            auto const pace = details::pointwise_tick_pace<Node>(ctx);
            auto const projected = details::project_tick_port(
                pace, ctx.index, ctx.block_size);
            auto const step = details::tick_lattice_step(pace);
            auto value_coordinate = projected.begin;
            for (size_t i = 0; i < projected.count; ++i) {
                do_tick(node, {
                    static_cast<TickContext<Node> const&>(ctx),
                    value_coordinate,
                    projected.begin_ordinal_bits + i,
                    i,
                });
                if (i + 1 < projected.count) {
                    value_coordinate = value_coordinate.advanced_by(step);
                }
            }
        }
    }

    template<typename Node>
    IV_FORCEINLINE void do_tick_block_batch(
        TickBlockBatchContext<Node> const& ctx)
    {
        if (ctx.empty()) return;
        auto const block_size = ctx[0].context().block_size;
        auto const index = ctx[0].context().index;
        if (block_size == 0) return;
        for (auto lane : ctx) {
            IV_ASSERT(lane.context().block_size == block_size,
                "Tick batch lanes have different block extents");
            IV_ASSERT(lane.context().index == index,
                "Tick batch lanes have different callback positions");
        }

        if constexpr (details::has_tick_block_batch<Node>) {
            Node::tick_block_batch(ctx);
        } else if constexpr (details::has_tick_block<Node>) {
            for (auto lane : ctx) {
                lane.node().tick_block(lane.context());
            }
        } else {
            static_assert(details::has_tick<Node>);
            auto const& first = ctx[0].context();
            auto const pace = details::pointwise_tick_pace<Node>(first);
            auto const projected = details::project_tick_port(
                pace, first.index, first.block_size);
            auto const step = details::tick_lattice_step(pace);
            auto value_coordinate = projected.begin;
            for (std::size_t sample = 0; sample < projected.count; ++sample) {
                for (auto lane : ctx) {
                    auto const& block = lane.context();
                    IV_ASSERT(
                        details::pointwise_tick_pace<Node>(block) == pace,
                        "pointwise Tick batch lanes have different port paces");
                    auto const lane_projection = details::project_tick_port(
                        pace, block.index, block.block_size);
                    IV_ASSERT(lane_projection.count == projected.count,
                        "pointwise Tick batch lanes have different block extents");
                    do_tick(lane.node(), TickSampleContext<Node>{
                        static_cast<TickContext<Node> const&>(block),
                        value_coordinate,
                        lane_projection.begin_ordinal_bits + sample,
                        sample,
                    });
                }
                if (sample + 1 < projected.count) {
                    value_coordinate = value_coordinate.advanced_by(step);
                }
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

        if constexpr (details::has_skip_block_batch<Node>)
        {
            details::TickBatchLaneBinding<Node, SkipBlockContext<Node>> lane{
                .node = std::addressof(node),
                .context = std::addressof(ctx),
            };
            SkipBlockBatchContext<Node> batch{{std::addressof(lane), 1}};
            Node::skip_block_batch(batch);
        }
        else if constexpr (details::has_skip_block<Node>)
        {
            node.skip_block(ctx);
        }
        else
        {
            details::author_default_skip_outputs<Node>(ctx);
        }

    }

    template<typename Node>
    IV_FORCEINLINE void do_skip_block_batch(
        SkipBlockBatchContext<Node> const& ctx)
    {
        if (ctx.empty()) return;
        auto const block_size = ctx[0].context().block_size;
        auto const index = ctx[0].context().index;
        if (block_size == 0) return;
        for (auto lane : ctx) {
            IV_ASSERT(lane.context().block_size == block_size,
                "skip batch lanes have different block extents");
            IV_ASSERT(lane.context().index == index,
                "skip batch lanes have different callback positions");
        }

        if constexpr (details::has_skip_block_batch<Node>) {
            Node::skip_block_batch(ctx);
        } else {
            for (auto lane : ctx) {
                do_skip_block(lane.node(), lane.context());
            }
        }
    }
}
