#pragma once

#include <intravenous/lane_node/traits.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

namespace iv {
    template<typename T>
    class SampleBlockView {
        std::span<T> _samples {};
        ChannelLayout _layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
        size_t _frame_count = 0;

        static constexpr size_t sample_offset(
            SampleStreamLayout layout,
            size_t frame,
            size_t channel,
            size_t frame_count,
            size_t channel_count_value) noexcept
        {
            if (layout == SampleStreamLayout::planar) {
                return channel * frame_count + frame;
            }
            return frame * channel_count_value + channel;
        }

    public:
        using value_type = std::remove_cv_t<T>;

        SampleBlockView() = default;

        explicit SampleBlockView(
            std::span<T> samples,
            ChannelLayout layout = {},
            size_t frame_count = 0) :
            _samples(samples),
            _layout(layout),
            _frame_count(frame_count == 0 ? samples.size() / channel_count(layout) : frame_count)
        {}

        std::span<T> samples() const
        {
            return _samples;
        }

        ChannelLayout channel_layout() const
        {
            return _layout;
        }

        ChannelTypeId channel_type() const
        {
            return _layout.channel_type;
        }

        SampleStreamLayout sample_layout() const
        {
            return _layout.sample_layout;
        }

        size_t channels() const
        {
            return channel_count(_layout);
        }

        size_t frames() const
        {
            return _frame_count;
        }

        size_t size() const
        {
            return _samples.size();
        }

        bool empty() const
        {
            return _samples.empty();
        }

        Sample get(size_t frame, size_t channel) const
        {
            if (frame >= frames() || channel >= channels()) {
                return Sample {};
            }
            return _samples[sample_offset(
                _layout.sample_layout,
                frame,
                channel,
                frames(),
                channels())];
        }

        template<typename U = T>
        requires (!std::is_const_v<U>)
        void set(size_t frame, size_t channel, Sample value) const
        {
            if (frame >= frames() || channel >= channels()) {
                return;
            }
            _samples[sample_offset(
                _layout.sample_layout,
                frame,
                channel,
                frames(),
                channels())] = value;
        }

        std::span<T> channel_span(size_t channel) const
        {
            if (_layout.sample_layout != SampleStreamLayout::planar || channel >= channels()) {
                return {};
            }
            return _samples.subspan(channel * frames(), frames());
        }

        T* interleaved_frame_ptr(size_t frame) const
        {
            if (_layout.sample_layout != SampleStreamLayout::interleaved || frame >= frames()) {
                return nullptr;
            }
            return _samples.data() + frame * channels();
        }
    };

    template<SampleStreamLayout Layout, typename T>
    class LayoutSpecializedSampleBlockView {
        SampleBlockView<T> _view {};

    public:
        LayoutSpecializedSampleBlockView() = default;
        explicit LayoutSpecializedSampleBlockView(SampleBlockView<T> view) :
            _view(view)
        {}

        SampleBlockView<T> view() const
        {
            return _view;
        }

        ChannelLayout channel_layout() const
        {
            return _view.channel_layout();
        }

        size_t frames() const
        {
            return _view.frames();
        }

        size_t channels() const
        {
            return _view.channels();
        }

        Sample get(size_t frame, size_t channel) const
        {
            return _view.get(frame, channel);
        }

        std::span<T> channel_span(size_t channel) const
        requires (Layout == SampleStreamLayout::planar)
        {
            return _view.channel_span(channel);
        }

        T* frame_ptr(size_t frame) const
        requires (Layout == SampleStreamLayout::interleaved)
        {
            return _view.interleaved_frame_ptr(frame);
        }
    };

    struct CompiledSampleLaneInput {
        std::vector<SampleBlockView<Sample const>> sources {};
        Sample default_value = 0.0f;
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
        size_t frame_count = 0;
        mutable std::vector<Sample> merged_storage {};

        bool connected() const
        {
            return !sources.empty();
        }

        SampleBlockView<Sample const> block_view() const
        {
            if (frame_count == 0) {
                return {};
            }
            if (sources.empty()) {
                merged_storage.assign(
                    sample_storage_size(channel_layout, frame_count),
                    default_value);
                return SampleBlockView<Sample const>(merged_storage, channel_layout, frame_count);
            }
            if (sources.size() == 1 && sources.front().channel_layout() == channel_layout) {
                return sources.front();
            }

            merged_storage.assign(
                sample_storage_size(channel_layout, frame_count),
                Sample {});
            auto merged = SampleBlockView<Sample>(merged_storage, channel_layout, frame_count);
            for (auto const& source : sources) {
                for (size_t frame = 0; frame < frame_count; ++frame) {
                    for (size_t channel = 0; channel < merged.channels(); ++channel) {
                        merged.set(
                            frame,
                            channel,
                            merged.get(frame, channel)
                                + (channel < source.channels() ? source.get(frame, channel) : default_value));
                    }
                }
            }
            return SampleBlockView<Sample const>(merged_storage, channel_layout, frame_count);
        }

        template<SampleStreamLayout Layout>
        auto block_view_as() const
        {
            return LayoutSpecializedSampleBlockView<Layout, Sample const>(block_view());
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
        std::vector<SampleBlockView<Sample const>> sources {};
        Sample default_value = 0.0f;
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
        size_t frame_count = 0;
        mutable std::vector<Sample> merged_storage {};

        bool connected() const
        {
            return !sources.empty();
        }

        SampleBlockView<Sample const> block_view() const
        {
            if (frame_count == 0) {
                return {};
            }
            if (sources.empty()) {
                merged_storage.assign(
                    sample_storage_size(channel_layout, frame_count),
                    default_value);
                return SampleBlockView<Sample const>(merged_storage, channel_layout, frame_count);
            }
            if (sources.size() == 1 && sources.front().channel_layout() == channel_layout) {
                return sources.front();
            }

            merged_storage.assign(
                sample_storage_size(channel_layout, frame_count),
                Sample {});
            auto merged = SampleBlockView<Sample>(merged_storage, channel_layout, frame_count);
            for (auto const& source : sources) {
                for (size_t frame = 0; frame < frame_count; ++frame) {
                    for (size_t channel = 0; channel < merged.channels(); ++channel) {
                        merged.set(
                            frame,
                            channel,
                            merged.get(frame, channel)
                                + (channel < source.channels() ? source.get(frame, channel) : default_value));
                    }
                }
            }
            return SampleBlockView<Sample const>(merged_storage, channel_layout, frame_count);
        }

        template<SampleStreamLayout Layout>
        auto block_view_as() const
        {
            return LayoutSpecializedSampleBlockView<Layout, Sample const>(block_view());
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
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };

        void write_block(size_t start_frame, SampleBlockView<Sample const> values)
        {
            if (!samples) {
                return;
            }
            size_t write_start_frame = start_frame;
            if (clamp_to_window) {
                if (start_frame < window_start_index) {
                    return;
                }
                write_start_frame -= window_start_index;
            }
            auto const total_samples = sample_storage_size(channel_layout, values.frames());
            if (channel_layout == values.channel_layout()) {
                size_t const write_start = write_start_frame * channel_count(channel_layout);
                if (samples->size() < write_start + total_samples) {
                    samples->resize(write_start + total_samples);
                }
                std::ranges::copy(
                    values.samples().first(total_samples),
                    samples->begin() + static_cast<std::ptrdiff_t>(write_start));
                return;
            }

            auto const start_index = write_start_frame * channel_count(channel_layout);
            if (samples->size() < start_index + total_samples) {
                samples->resize(start_index + total_samples);
            }
            SampleBlockView<Sample> dst(
                std::span<Sample>(*samples).subspan(start_index, total_samples),
                channel_layout,
                values.frames());
            auto const frame_count = std::min(dst.frames(), values.frames());
            auto const dst_channels = dst.channels();
            auto const src_channels = values.channels();
            for (size_t frame = 0; frame < frame_count; ++frame) {
                for (size_t channel = 0; channel < std::min(dst_channels, src_channels); ++channel) {
                    dst.set(frame, channel, values.get(frame, channel));
                }
                for (size_t channel = src_channels; channel < dst_channels; ++channel) {
                    dst.set(frame, channel, Sample {});
                }
            }
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
        SampleBlockView<Sample> _block {};

    public:
        RealtimeSampleLaneOutput() = default;
        explicit RealtimeSampleLaneOutput(SampleBlockView<Sample> block) :
            _block(block)
        {}

        ChannelLayout channel_layout() const
        {
            return _block.channel_layout();
        }

        SampleBlockView<Sample> block_view() const
        {
            return _block;
        }

        template<SampleStreamLayout Layout>
        auto block_view_as() const
        {
            return LayoutSpecializedSampleBlockView<Layout, Sample>(block_view());
        }

        void write_block(SampleBlockView<Sample const> values)
        {
            auto const dst = block_view();
            auto const frame_count = std::min(dst.frames(), values.frames());
            auto const dst_channels = dst.channels();
            auto const src_channels = values.channels();
            for (size_t frame = 0; frame < frame_count; ++frame) {
                for (size_t channel = 0; channel < std::min(dst_channels, src_channels); ++channel) {
                    dst.set(frame, channel, values.get(frame, channel));
                }
                for (size_t channel = src_channels; channel < dst_channels; ++channel) {
                    dst.set(frame, channel, Sample {});
                }
            }
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
        template<typename OutputView>
        ChannelLayout output_channel_layout(OutputView const& output)
        {
            if constexpr (requires { output.channel_layout(); }) {
                return output.channel_layout();
            } else if constexpr (requires { output.channel_layout; }) {
                return output.channel_layout;
            } else {
                return ChannelLayout{
                    .channel_type = ChannelTypeId::mono,
                    .sample_layout = SampleStreamLayout::planar,
                };
            }
        }

        template<typename LaneNode>
        void invoke_compiled_from_realtime(LaneNode& node, RealtimeLaneTickContext<LaneNode>& ctx)
        {
            auto const compiled_sample_decls = get_compiled_sample_lane_inputs(node);
            auto const compiled_event_decls = get_compiled_event_lane_inputs(node);
            std::vector<CompiledSampleLaneInput> compiled_sample_inputs_storage;
            compiled_sample_inputs_storage.resize(compiled_sample_decls.size());
            for (size_t i = 0; i < compiled_sample_decls.size(); ++i) {
                compiled_sample_inputs_storage[i].default_value = compiled_sample_decls[i].default_value;
                auto const out_layout = output_channel_layout(ctx.out());
                compiled_sample_inputs_storage[i].channel_layout = ChannelLayout {
                    .channel_type = out_layout.channel_type,
                    .sample_layout = compiled_sample_decls[i].sample_layout,
                };
                compiled_sample_inputs_storage[i].frame_count = ctx.sample_count();
            }
            for (size_t i = 0; i < std::min(ctx.realtime_sample_inputs().size(), compiled_sample_inputs_storage.size()); ++i) {
                auto const& input = ctx.realtime_sample_inputs()[i];
                if (input.connected()) {
                    compiled_sample_inputs_storage[i].sources.push_back(input.block_view());
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
