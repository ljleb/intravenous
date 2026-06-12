#pragma once

#include <intravenous/lane_node/traits.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace iv {
    struct CompiledSampleLaneInput {
        std::vector<std::span<Sample const>> sources {};
        Sample default_value = 0.0f;

        Sample sample_at(size_t index) const
        {
            if (sources.empty()) {
                return default_value;
            }
            Sample value = 0.0f;
            for (auto const source : sources) {
                if (index < source.size()) {
                    value += source[index];
                }
            }
            return value;
        }

        void read(size_t start_index, std::span<Sample> out) const
        {
            if (sources.empty()) {
                std::ranges::fill(out, default_value);
                return;
            }
            std::ranges::fill(out, 0.0f);
            for (auto const source : sources) {
                if (start_index >= source.size()) {
                    continue;
                }
                size_t const available = std::min(out.size(), source.size() - start_index);
                for (size_t i = 0; i < available; ++i) {
                    out[i] += source[start_index + i];
                }
            }
        }
    };

    struct CompiledEventLaneInput {
        std::vector<std::span<TimedEvent const>> sources {};
        mutable std::vector<TimedEvent> merged {};

        std::span<TimedEvent const> events(size_t start_index, size_t count) const
        {
            merged.clear();
            size_t const end_index = start_index + count;
            for (auto const source : sources) {
                for (auto const& event : source) {
                    if (event.time >= start_index && event.time < end_index) {
                        merged.push_back(event);
                    }
                }
            }
            std::ranges::sort(merged, {}, &TimedEvent::time);
            return merged;
        }
    };

    struct RealtimeSampleLaneInput {
        using GetBlockFn = std::span<Sample const> (*)(void*, size_t, size_t);

        void* context = nullptr;
        GetBlockFn get_block_fn = nullptr;
        Sample default_value = 0.0f;
        size_t active_start_index = 0;
        size_t active_count = 0;
        bool has_source = false;
        std::span<Sample const> block_override {};

        bool connected() const
        {
            return has_source;
        }

        Sample get(size_t index) const
        {
            auto const block = get_block(active_start_index, active_count);
            if (index >= block.size()) {
                return default_value;
            }
            return block[index];
        }

        std::span<Sample const> get_block(size_t start_index, size_t count) const
        {
            if (!block_override.empty()) {
                if (start_index != active_start_index) {
                    return {};
                }
                return block_override.first(std::min(count, block_override.size()));
            }
            if (!get_block_fn) {
                return {};
            }
            return get_block_fn(context, start_index, count);
        }

        std::span<Sample const> get_block() const
        {
            return get_block(active_start_index, active_count);
        }
    };

    struct RealtimeEventLaneInput {
        using GetEventsFn = std::span<TimedEvent const> (*)(void*, size_t, size_t);

        void* context = nullptr;
        GetEventsFn get_events_fn = nullptr;
        size_t active_start_index = 0;
        size_t active_count = 0;
        std::span<TimedEvent const> block_override {};

        std::span<TimedEvent const> get_block(size_t start_index, size_t count) const
        {
            if (!block_override.empty()) {
                if (start_index != active_start_index) {
                    return {};
                }
                return block_override.first(std::min(count, block_override.size()));
            }
            if (!get_events_fn) {
                return {};
            }
            return get_events_fn(context, start_index, count);
        }

        std::span<TimedEvent const> get_block() const
        {
            return get_block(active_start_index, active_count);
        }
    };

    struct CompiledSampleLaneOutput {
        std::vector<Sample>* samples = nullptr;
        size_t window_start_index = 0;
        bool clamp_to_window = false;

        void write(size_t start_index, std::span<Sample const> values)
        {
            if (!samples) {
                return;
            }
            size_t write_start = start_index;
            if (clamp_to_window) {
                if (start_index < window_start_index) {
                    size_t const delta = window_start_index - start_index;
                    if (delta >= values.size()) {
                        return;
                    }
                    values = values.subspan(delta);
                    write_start = 0;
                } else {
                    write_start = start_index - window_start_index;
                }
                if (write_start >= samples->size()) {
                    return;
                }
                values = values.first(std::min(values.size(), samples->size() - write_start));
            } else if (samples->size() < start_index + values.size()) {
                samples->resize(start_index + values.size());
            }
            std::ranges::copy(values, samples->begin() + static_cast<std::ptrdiff_t>(write_start));
        }

        template<size_t N>
        void write(size_t start_index, std::array<Sample, N> const& values)
        {
            write(start_index, std::span<Sample const>(values));
        }
    };

    struct CompiledEventLaneOutput {
        std::vector<TimedEvent>* events = nullptr;

        void push(TimedEvent const& event)
        {
            if (!events) {
                return;
            }
            events->push_back(event);
        }

        void push_block(std::span<TimedEvent const> block)
        {
            if (!events) {
                return;
            }
            events->insert(events->end(), block.begin(), block.end());
        }
    };

    class RealtimeSampleLaneOutput {
        BlockView<Sample> _block {};
        size_t _position = 0;

    public:
        RealtimeSampleLaneOutput() = default;
        explicit RealtimeSampleLaneOutput(BlockView<Sample> block) :
            _block(block)
        {}

        void push(Sample value)
        {
            if (_position >= _block.size()) {
                return;
            }
            _block[_position++] = value;
        }

        void push_block(BlockView<Sample const> samples)
        {
            for (size_t i = 0; i < samples.size(); ++i) {
                push(samples[i]);
            }
        }

        BlockView<Sample> block() const
        {
            return _block;
        }
    };

    class RealtimeEventLaneOutput {
        std::vector<TimedEvent>* _events = nullptr;

    public:
        RealtimeEventLaneOutput() = default;
        explicit RealtimeEventLaneOutput(std::vector<TimedEvent>& events) :
            _events(&events)
        {}

        void push(TimedEvent const& event) const
        {
            if (!_events) {
                return;
            }
            _events->push_back(event);
        }

        void push_block(BlockView<TimedEvent const> events) const
        {
            for (TimedEvent const& event : events) {
                push(event);
            }
        }
    };

    using LaneOutputView = std::variant<
        CompiledSampleLaneOutput,
        CompiledEventLaneOutput,
        RealtimeSampleLaneOutput,
        RealtimeEventLaneOutput
    >;

    template<typename OutputDeclaration>
    struct LaneOutputViewFor {
        using Type = LaneOutputView;
    };

    template<>
    struct LaneOutputViewFor<CompiledSampleLaneOutputConfig> {
        using Type = CompiledSampleLaneOutput;
    };

    template<>
    struct LaneOutputViewFor<CompiledEventLaneOutputConfig> {
        using Type = CompiledEventLaneOutput;
    };

    template<>
    struct LaneOutputViewFor<RealtimeSampleLaneOutputConfig> {
        using Type = RealtimeSampleLaneOutput;
    };

    template<>
    struct LaneOutputViewFor<RealtimeEventLaneOutputConfig> {
        using Type = RealtimeEventLaneOutput;
    };

    template<>
    struct LaneOutputViewFor<LaneOutputConfig> {
        using Type = LaneOutputView;
    };

    struct TimelineOutputRequest {
        size_t start_index = 0;
        size_t count = 0;

        bool operator==(TimelineOutputRequest const&) const = default;
    };

    struct CompiledLaneTickRequest {
        size_t start_index = 0;
        size_t end_index = 0;
        size_t sample_count = 0;
    };

    struct RealtimeLaneTickRequest {
        size_t start_index = 0;
        size_t sample_count = 0;
    };

    struct CompiledSupportRange {
        size_t start_index = 0;
        size_t end_index = 0;

        bool operator==(CompiledSupportRange const&) const = default;
    };

    struct CompiledSupportLaneInput {
        std::vector<std::span<CompiledSupportRange const>> sources {};
    };

    struct UntypedCompiledSupportContext {
        std::span<CompiledSupportLaneInput> compiled_sample_inputs {};
        std::span<CompiledSupportLaneInput> compiled_event_inputs {};
    };

    struct LaneInputConnectivityDescription {
        std::uint64_t source_lane_value = 0;
        LanePortDomain source_output_domain = LanePortDomain::realtime;
        PortKind kind = PortKind::sample;
        size_t input_ordinal = 0;
        std::span<CompiledSupportRange const> compiled_support_ranges {};
    };

    struct UntypedInputsChangedContext {
        std::span<LaneInputConnectivityDescription const> inputs {};
    };

    struct UntypedCompiledLaneTickContext {
        CompiledLaneTickRequest request {};
        std::span<CompiledSampleLaneInput> compiled_sample_inputs {};
        std::span<CompiledEventLaneInput> compiled_event_inputs {};
        LaneOutputView output { CompiledSampleLaneOutput {} };
    };

    struct UntypedRealtimeLaneTickContext {
        RealtimeLaneTickRequest request {};
        std::optional<CompiledLaneTickRequest> compiled_fallback_tick_window {};
        std::span<CompiledSampleLaneInput> compiled_sample_inputs {};
        std::span<CompiledEventLaneInput> compiled_event_inputs {};
        std::span<RealtimeSampleLaneInput> realtime_sample_inputs {};
        std::span<RealtimeEventLaneInput> realtime_event_inputs {};
        LaneOutputView output { RealtimeSampleLaneOutput {} };
    };

    template<typename LaneNode>
    class CompiledLaneTickContext {
        UntypedCompiledLaneTickContext& _untyped;

        template<typename>
        friend class CompiledLaneTickContext;
        template<typename>
        friend class RealtimeLaneTickContext;

    public:
        using OutputView = typename LaneOutputViewFor<
            std::remove_cvref_t<LaneOutputDeclaration<LaneNode>>
        >::Type;

        explicit CompiledLaneTickContext(UntypedCompiledLaneTickContext& untyped) :
            _untyped(untyped)
        {}

        size_t start_index() const { return _untyped.request.start_index; }
        size_t end_index() const { return _untyped.request.end_index; }
        size_t sample_count() const { return _untyped.request.sample_count; }
        std::span<CompiledSampleLaneInput> compiled_sample_inputs() const { return _untyped.compiled_sample_inputs; }
        std::span<CompiledEventLaneInput> compiled_event_inputs() const { return _untyped.compiled_event_inputs; }
        CompiledSampleLaneInput& compiled_sample_input(size_t index) const { return _untyped.compiled_sample_inputs[index]; }
        CompiledEventLaneInput& compiled_event_input(size_t index) const { return _untyped.compiled_event_inputs[index]; }

        OutputView& out() const
        {
            if constexpr (std::same_as<OutputView, LaneOutputView>) {
                return _untyped.output;
            } else {
                return std::get<OutputView>(_untyped.output);
            }
        }

        UntypedCompiledLaneTickContext& untyped() const
        {
            return _untyped;
        }
    };

    template<typename LaneNode>
    class CompiledSupportContext {
        UntypedCompiledSupportContext& _untyped;

    public:
        explicit CompiledSupportContext(UntypedCompiledSupportContext& untyped) :
            _untyped(untyped)
        {}

        std::span<CompiledSupportLaneInput> compiled_sample_inputs() const { return _untyped.compiled_sample_inputs; }
        std::span<CompiledSupportLaneInput> compiled_event_inputs() const { return _untyped.compiled_event_inputs; }
        CompiledSupportLaneInput& compiled_sample_input(size_t index) const { return _untyped.compiled_sample_inputs[index]; }
        CompiledSupportLaneInput& compiled_event_input(size_t index) const { return _untyped.compiled_event_inputs[index]; }
        UntypedCompiledSupportContext& untyped() const { return _untyped; }
    };

    template<typename LaneNode>
    class InputsChangedContext {
        UntypedInputsChangedContext& _untyped;

    public:
        explicit InputsChangedContext(UntypedInputsChangedContext& untyped) :
            _untyped(untyped)
        {}

        std::span<LaneInputConnectivityDescription const> inputs() const { return _untyped.inputs; }
        UntypedInputsChangedContext& untyped() const { return _untyped; }
    };

    template<typename LaneNode>
    class RealtimeLaneTickContext {
        UntypedRealtimeLaneTickContext& _untyped;

        template<typename>
        friend class CompiledLaneTickContext;
        template<typename>
        friend class RealtimeLaneTickContext;

    public:
        using OutputView = typename LaneOutputViewFor<
            std::remove_cvref_t<LaneOutputDeclaration<LaneNode>>
        >::Type;

        explicit RealtimeLaneTickContext(UntypedRealtimeLaneTickContext& untyped) :
            _untyped(untyped)
        {}

        size_t start_index() const { return _untyped.request.start_index; }
        size_t sample_count() const { return _untyped.request.sample_count; }
        std::span<CompiledSampleLaneInput> compiled_sample_inputs() const { return _untyped.compiled_sample_inputs; }
        std::span<CompiledEventLaneInput> compiled_event_inputs() const { return _untyped.compiled_event_inputs; }
        std::span<RealtimeSampleLaneInput> realtime_sample_inputs() const { return _untyped.realtime_sample_inputs; }
        std::span<RealtimeEventLaneInput> realtime_event_inputs() const { return _untyped.realtime_event_inputs; }
        CompiledSampleLaneInput& compiled_sample_input(size_t index) const { return _untyped.compiled_sample_inputs[index]; }
        CompiledEventLaneInput& compiled_event_input(size_t index) const { return _untyped.compiled_event_inputs[index]; }
        RealtimeSampleLaneInput& realtime_sample_input(size_t index) const { return _untyped.realtime_sample_inputs[index]; }
        RealtimeEventLaneInput& realtime_event_input(size_t index) const { return _untyped.realtime_event_inputs[index]; }

        OutputView& out() const
        {
            if constexpr (std::same_as<OutputView, LaneOutputView>) {
                return _untyped.output;
            } else {
                return std::get<OutputView>(_untyped.output);
            }
        }

        UntypedRealtimeLaneTickContext& untyped() const
        {
            return _untyped;
        }
    };

    namespace lane_node_details {
        template<typename LaneNode>
        concept has_tick_block_compiled = requires(LaneNode& node, CompiledLaneTickContext<LaneNode>& ctx) {
            node.tick_block_compiled(ctx);
        };

        template<typename LaneNode>
        concept has_tick_block_realtime = requires(LaneNode& node, RealtimeLaneTickContext<LaneNode>& ctx) {
            node.tick_block_realtime(ctx);
        };

        template<typename LaneNode>
        concept has_compiled_support_ranges = requires(LaneNode& node, CompiledSupportContext<LaneNode>& ctx) {
            node.compiled_support_ranges(ctx);
        };

        template<typename LaneNode>
        concept has_on_inputs_changed = requires(LaneNode& node, InputsChangedContext<LaneNode>& ctx) {
            node.on_inputs_changed(ctx);
        };
    } // namespace lane_node_details

    namespace lane_tick_details {
        template<typename LaneNode>
        void invoke_compiled_from_realtime(LaneNode& node, RealtimeLaneTickContext<LaneNode>& ctx)
        {
            auto const compiled_sample_decls = get_compiled_sample_lane_inputs(node);
            auto const compiled_event_decls = get_compiled_event_lane_inputs(node);
            std::vector<CompiledSampleLaneInput> compiled_sample_inputs_storage;
            compiled_sample_inputs_storage.resize(compiled_sample_decls.size());
            for (size_t i = 0; i < compiled_sample_decls.size(); ++i) {
                compiled_sample_inputs_storage[i].default_value = compiled_sample_decls[i].default_value;
            }
            for (size_t i = 0; i < std::min(ctx.realtime_sample_inputs().size(), compiled_sample_inputs_storage.size()); ++i) {
                auto const& input = ctx.realtime_sample_inputs()[i];
                if (input.connected()) {
                    compiled_sample_inputs_storage[i].sources.push_back(input.get_block());
                }
                compiled_sample_inputs_storage[i].default_value = input.default_value;
            }
            for (size_t i = 0; i < std::min(ctx.compiled_sample_inputs().size(), compiled_sample_inputs_storage.size()); ++i) {
                compiled_sample_inputs_storage[i] = ctx.compiled_sample_inputs()[i];
            }

            std::vector<CompiledEventLaneInput> compiled_event_inputs_storage;
            compiled_event_inputs_storage.resize(compiled_event_decls.size());
            for (size_t i = 0; i < std::min(ctx.realtime_event_inputs().size(), compiled_event_inputs_storage.size()); ++i) {
                auto const& input = ctx.realtime_event_inputs()[i];
                auto const block = input.get_block();
                if (!block.empty()) {
                    compiled_event_inputs_storage[i].sources.push_back(block);
                }
            }
            for (size_t i = 0; i < std::min(ctx.compiled_event_inputs().size(), compiled_event_inputs_storage.size()); ++i) {
                compiled_event_inputs_storage[i] = ctx.compiled_event_inputs()[i];
            }

            UntypedCompiledLaneTickContext untyped_compiled {
                .request = ctx.untyped().compiled_fallback_tick_window.value_or(CompiledLaneTickRequest {
                    .start_index = ctx.start_index(),
                    .end_index = ctx.start_index() + ctx.sample_count(),
                    .sample_count = ctx.sample_count(),
                }),
                .compiled_sample_inputs = compiled_sample_inputs_storage,
                .compiled_event_inputs = compiled_event_inputs_storage,
                .output = ctx.out(),
            };
            CompiledLaneTickContext<LaneNode> compiled_ctx(untyped_compiled);
            node.tick_block_compiled(compiled_ctx);
        }
    }

    template<typename LaneNode>
    constexpr bool supports_tick_block_compiled()
    {
        return lane_node_details::has_tick_block_compiled<LaneNode>;
    }

    template<typename LaneNode>
    constexpr bool supports_tick_block_realtime()
    {
        return lane_node_details::has_tick_block_realtime<LaneNode>
            || lane_node_details::has_tick_block_compiled<LaneNode>;
    }

    template<typename LaneNode>
    bool supports_tick_block_compiled(LaneNode const& node)
    {
        if constexpr (requires(LaneNode const& node) { node.supports_tick_block_compiled(); }) {
            return node.supports_tick_block_compiled();
        } else {
            return supports_tick_block_compiled<LaneNode>();
        }
    }

    template<typename LaneNode>
    bool supports_tick_block_realtime(LaneNode const& node)
    {
        if constexpr (requires(LaneNode const& node) { node.supports_tick_block_realtime(); }) {
            return node.supports_tick_block_realtime();
        } else {
            return supports_tick_block_realtime<LaneNode>();
        }
    }

    template<typename LaneNode>
    constexpr bool has_compiled_support_ranges()
    {
        return lane_node_details::has_compiled_support_ranges<LaneNode>;
    }

    template<typename LaneNode>
    bool supports_compiled_support_ranges(LaneNode const& node)
    {
        if constexpr (requires(LaneNode const& node) { node.supports_compiled_support_ranges(); }) {
            return node.supports_compiled_support_ranges();
        } else {
            return has_compiled_support_ranges<LaneNode>();
        }
    }

    template<typename LaneNode>
    constexpr bool has_on_inputs_changed()
    {
        return lane_node_details::has_on_inputs_changed<LaneNode>;
    }

    template<typename LaneNode>
    auto get_compiled_support_ranges(LaneNode& node, CompiledSupportContext<LaneNode>& ctx)
    {
        if constexpr (lane_node_details::has_compiled_support_ranges<LaneNode>) {
            return node.compiled_support_ranges(ctx);
        } else {
            static_assert(
                lane_node_details::has_compiled_support_ranges<LaneNode>,
                "compiled-capable lane node must define compiled_support_ranges()");
        }
    }

    template<typename LaneNode>
    void do_on_inputs_changed(LaneNode& node, InputsChangedContext<LaneNode>& ctx)
    {
        if constexpr (lane_node_details::has_on_inputs_changed<LaneNode>) {
            node.on_inputs_changed(ctx);
        }
    }

    template<typename LaneNode>
    void do_tick_block_compiled(LaneNode& node, CompiledLaneTickContext<LaneNode>& ctx)
    {
        if constexpr (lane_node_details::has_tick_block_compiled<LaneNode>) {
            node.tick_block_compiled(ctx);
        } else {
            static_assert(lane_node_details::has_tick_block_compiled<LaneNode>, "lane node must define tick_block_compiled()");
        }
    }

    template<typename LaneNode>
    void do_tick_block_realtime(LaneNode& node, RealtimeLaneTickContext<LaneNode>& ctx)
    {
        if constexpr (lane_node_details::has_tick_block_realtime<LaneNode> && !lane_node_details::has_tick_block_compiled<LaneNode>) {
            node.tick_block_realtime(ctx);
        } else if constexpr (!lane_node_details::has_tick_block_realtime<LaneNode> && lane_node_details::has_tick_block_compiled<LaneNode>) {
            lane_tick_details::invoke_compiled_from_realtime(node, ctx);
        } else if constexpr (lane_node_details::has_tick_block_realtime<LaneNode> && lane_node_details::has_tick_block_compiled<LaneNode>) {
            if (get_lane_output_domain(node) == LanePortDomain::compiled) {
                lane_tick_details::invoke_compiled_from_realtime(node, ctx);
            } else {
                node.tick_block_realtime(ctx);
            }
        } else {
            static_assert(
                lane_node_details::has_tick_block_realtime<LaneNode>
                    || lane_node_details::has_tick_block_compiled<LaneNode>,
                "lane node must define tick_block_realtime() or tick_block_compiled()");
        }
    }

}
