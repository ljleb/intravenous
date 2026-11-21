namespace {
// Background-evaluated sources whose playback the tests supply directly as a
// TickMaterializationSnapshot. Their own evaluation is covered elsewhere.
struct PlaybackSampleSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::coverage)};
    }

    void tock_coverage(iv::TockCoverageContext<PlaybackSampleSource>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<PlaybackSampleSource>&) const {}
};

struct PlaybackTriggerSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            "out",
            iv::EventTypeId::trigger,
            iv::OutputProduction::coverage,
            {.max_events_per_index = 0.25})};
    }

    void tock_coverage(iv::TockCoverageContext<PlaybackTriggerSource>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<PlaybackTriggerSource>&) const {}
};

// Records its complete visible history window: window[k] = get(4 - k).
struct PlaybackHistoryConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::array<float, 5> window{};
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PlaybackHistoryConsumer>& ctx) const
    {
        ctx.input<"in">().history() = 4;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<PlaybackHistoryConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        for (std::size_t k = 0; k < state.window.size(); ++k) {
            state.window[k] = static_cast<float>(
                ctx.template input<0>().get(state.window.size() - 1 - k));
        }
    }
};

// A second consumer type, so a test can tell two consumers apart by state
// size.
struct PlaybackHistoryPeer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::array<float, 5> window{};
        std::uint64_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    void constrain_ports(iv::ConstrainPortsContext<PlaybackHistoryPeer>& ctx) const
    {
        ctx.input<"in">().history() = 4;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<PlaybackHistoryPeer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        for (std::size_t k = 0; k < state.window.size(); ++k) {
            state.window[k] = static_cast<float>(
                ctx.template input<0>().get(state.window.size() - 1 - k));
        }
        state.marker = 0x91e3u;
    }
};

// left[k] and right[k] = get(2 - k, channel).
struct PlaybackStereoHistoryConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::array<float, 3> left{};
        std::array<float, 3> right{};
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential,
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PlaybackStereoHistoryConsumer>& ctx) const
    {
        ctx.input<"in">().history() = 2;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<PlaybackStereoHistoryConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        for (std::size_t k = 0; k < state.left.size(); ++k) {
            auto const history = state.left.size() - 1 - k;
            state.left[k] = static_cast<float>(
                ctx.template input<0>().get(history, 0));
            state.right[k] = static_cast<float>(
                ctx.template input<0>().get(history, 1));
        }
    }
};

// Records the event times of its window [index - 32, index + block).
struct PlaybackEventHistoryConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t count = 0;
        std::array<std::uint64_t, 8> times{};
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PlaybackEventHistoryConsumer>& ctx) const
    {
        ctx.input<"trigger">().history() = 32;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<PlaybackEventHistoryConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        constexpr std::size_t history = 32;
        auto const events = ctx.template input<0>().events(
            ctx.index.retreated_by({.whole = history}).whole(),
            ctx.block_size + history);
        state.count = events.size();
        for (std::size_t i = 0; i < state.times.size(); ++i) {
            state.times[i] = i < events.size() ? events[i].time.bits() : 0;
        }
    }
};

struct PlaybackEventHistoryPeer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t count = 0;
        std::array<std::uint64_t, 8> times{};
        std::uint64_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PlaybackEventHistoryPeer>& ctx) const
    {
        ctx.input<"trigger">().history() = 32;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<PlaybackEventHistoryPeer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        constexpr std::size_t history = 32;
        auto const events = ctx.template input<0>().events(
            ctx.index.retreated_by({.whole = history}).whole(),
            ctx.block_size + history);
        state.count = events.size();
        for (std::size_t i = 0; i < state.times.size(); ++i) {
            state.times[i] = i < events.size() ? events[i].time.bits() : 0;
        }
        state.marker = 0x91e4u;
    }
};

struct ModuleBoundaryCoverageObserver {
    struct alignas(64) State {
        std::uint64_t calls = 0;
        std::uint64_t sample_region_count = 0;
        std::uint64_t event_region_count = 0;
        std::uint64_t first_event_count = 0;
        std::uint64_t second_event_count = 0;
        float first_overlap_value = 0.0f;
        float second_overlap_value = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::tuple{
            iv::input<iv::Sample>(
                "module-boundary-sample-input",
                iv::InputAccess::coverage),
            iv::event_input(
                "module-boundary-event-input",
                iv::EventTypeId::trigger,
                iv::InputAccess::coverage),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{
            iv::output<iv::Sample>(
                "module-boundary-sample-output",
                iv::OutputProduction::coverage),
            iv::event_output(
                "module-boundary-event-output",
                iv::EventTypeId::trigger,
                iv::OutputProduction::coverage,
                {.max_events_per_index = 1.0}),
        };
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<
            ModuleBoundaryCoverageObserver>& ctx) const
    {
        auto samples = ctx.input<"module-boundary-sample-input">();
        ctx.output<"module-boundary-sample-output">().publish(
            samples.regions());
        ctx.output<"module-boundary-sample-output">().change(
            samples.changed());
        auto events = ctx.input<"module-boundary-event-input">();
        ctx.output<"module-boundary-event-output">().publish(
            events.regions());
        ctx.output<"module-boundary-event-output">().change(
            events.changed());
    }

    void propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<
            ModuleBoundaryCoverageObserver>& ctx) const
    {
        for (auto region : ctx.input<"module-boundary-sample-input">().coverage()) ctx.input<"module-boundary-sample-input">().require(region);
        for (auto region : ctx.input<"module-boundary-event-input">().coverage()) ctx.input<"module-boundary-event-input">().require(region);
    }

    void tock_coverage(
        iv::TockCoverageContext<ModuleBoundaryCoverageObserver>& ctx) const
    {
        auto samples = ctx.input<"module-boundary-sample-input">().regions();
        auto sample_outputs = ctx.output<"module-boundary-sample-output">();
        auto events = ctx.input<"module-boundary-event-input">().regions();
        auto event_outputs =
            ctx.output<"module-boundary-event-output">().regions();
        IV_ASSERT(samples.size() == 2 && sample_outputs.regions().size() == 2,
            "module boundary did not preserve two sample Regions");
        IV_ASSERT(events.size() == 2 && event_outputs.size() == 2,
            "module boundary did not preserve two event Regions");

        auto& state = ctx.state();
        ++state.calls;
        state.sample_region_count = samples.size();
        state.event_region_count = events.size();
        auto const overlap = iv::GlobalIndex::from_signed(12);
        state.first_overlap_value = static_cast<float>(samples[0].at(overlap));
        state.second_overlap_value = static_cast<float>(samples[1].at(overlap));
        events[0].for_each([&](iv::TimedEvent const&) {
            ++state.first_event_count;
        });
        events[1].for_each([&](iv::TimedEvent const&) {
            ++state.second_event_count;
        });

        for (auto run : sample_outputs.prepared_runs())
            for (std::size_t slot = 0; slot < run.size(); ++slot)
                run.value(slot) = samples[run.region_index()].at(run.position(slot));
        for (std::size_t region = 0; region < events.size(); ++region) {
            events[region].for_each([&](iv::TimedEvent const& event) {
                if (ctx.output<"module-boundary-event-output">().requested_coverage().contains(event.time)) event_outputs[region].write(event);
            });
        }
    }
};

void multi_region_boundary_child_module(iv::GraphBuilder& graph)
{
    auto sample_input = graph.input<"samples", iv::Sample>(iv::InputAccess::coverage);
    auto event_input = graph.event_input<"events">(
        iv::EventTypeId::trigger, iv::InputAccess::coverage);
    auto sample = graph.node<
        "iv.test.graph_jit.state_context.multi_region_coverage_consumer">();
    auto events = graph.node<
        "iv.test.graph_jit.state_context.multi_region_event_coverage_consumer">();
    sample(sample_input);
    events.connect_event_input(0, event_input);
    graph.outputs(sample);
    graph.event_outputs(events.event_port());
}

struct PersistedStereoRecordingSource {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::tuple{
            iv::output<iv::Sample>(
                "recorded-samples",
                iv::OutputProduction::sequential,
                iv::OutputRealization::buffered,
                {.channel_layout = {
                    .channel_type = iv::ChannelTypeId::stereo,
                    .sample_layout = iv::SampleStreamLayout::planar,
                }}),
            iv::event_output(
                "recorded-events",
                iv::EventTypeId::trigger,
                iv::OutputProduction::sequential,
                iv::OutputRealization::buffered,
                {.max_events_per_index = 0.25}),
        };
    }

    void tick_block(
        iv::TickBlockContext<PersistedStereoRecordingSource> const& ctx) const
    {
        auto samples = ctx.output<"recorded-samples">();
        for (std::size_t frame = 0; frame < ctx.block_size; ++frame) {
            auto const position = ctx.index.advanced_by({.whole = frame}).whole();
            samples.value(frame, 0) =
                iv::Sample{static_cast<float>(position.signed_value())};
            samples.value(frame, 1) = iv::Sample{
                1000.0f + static_cast<float>(position.signed_value())};
        }
        if (ctx.block_size > 2) {
            ctx.output<"recorded-events">().push(iv::TriggerEvent{}, 2);
        }
    }
};

struct PersistedRecordingCoverageConsumer {
    struct alignas(64) State {
        std::uint64_t calls = 0;
        std::uint64_t sample_region_count = 0;
        std::uint64_t sample_layout = 0;
        std::uint64_t event_region_count = 0;
        std::uint64_t event_count = 0;
        std::uint64_t first_event_time = 0;
        float first_left = 0.0f;
        float first_right = 0.0f;
        float last_left = 0.0f;
        float last_right = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::tuple{
            iv::input<iv::Sample>(
                "recorded-sample-coverage",
                iv::InputAccess::coverage,
                {.channel_layout = {
                    .channel_type = iv::ChannelTypeId::stereo,
                    .sample_layout = iv::SampleStreamLayout::planar,
                }}),
            iv::event_input(
                "recorded-event-coverage",
                iv::EventTypeId::trigger,
                iv::InputAccess::coverage),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{
            iv::output<iv::Sample>(
                "observed-sample-coverage",
                iv::OutputProduction::coverage,
                {.channel_layout = {
                    .channel_type = iv::ChannelTypeId::stereo,
                    .sample_layout = iv::SampleStreamLayout::planar,
                }}),
            iv::event_output(
                "observed-event-coverage",
                iv::EventTypeId::trigger,
                iv::OutputProduction::coverage,
                {.max_events_per_index = 0.25}),
        };
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<
            PersistedRecordingCoverageConsumer>& ctx) const
    {
        auto samples = ctx.input<"recorded-sample-coverage">();
        ctx.output<"observed-sample-coverage">().publish(samples.regions());
        ctx.output<"observed-sample-coverage">().change(samples.changed());
        auto events = ctx.input<"recorded-event-coverage">();
        ctx.output<"observed-event-coverage">().publish(events.regions());
        ctx.output<"observed-event-coverage">().change(events.changed());
    }

    void propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<
            PersistedRecordingCoverageConsumer>& ctx) const
    {
        for (auto region : ctx.input<"recorded-sample-coverage">().coverage()) ctx.input<"recorded-sample-coverage">().require(region);
        for (auto region : ctx.input<"recorded-event-coverage">().coverage()) ctx.input<"recorded-event-coverage">().require(region);
    }

    void tock_coverage(
        iv::TockCoverageContext<PersistedRecordingCoverageConsumer>& ctx) const
    {
        auto sample_inputs =
            ctx.input<"recorded-sample-coverage">().regions();
        auto sample_outputs = ctx.output<"observed-sample-coverage">();
        auto event_inputs =
            ctx.input<"recorded-event-coverage">().regions();
        auto event_outputs =
            ctx.output<"observed-event-coverage">().regions();
        IV_ASSERT(sample_inputs.size() == sample_outputs.regions().size(),
            "recorded sample input and output Region counts differ");
        IV_ASSERT(event_inputs.size() == event_outputs.size(),
            "recorded event input and output Region counts differ");

        auto& state = ctx.state();
        ++state.calls;
        state.sample_region_count = sample_inputs.size();
        state.event_region_count = event_inputs.size();
        if (!sample_inputs.empty()) {
            auto const first = sample_inputs[0];
            auto const last = sample_inputs[sample_inputs.size() - 1];
            state.sample_layout = static_cast<std::uint64_t>(
                first.channel_layout().sample_layout);
            state.first_left = static_cast<float>(first.value(0, 0));
            state.first_right = static_cast<float>(first.value(0, 1));
            state.last_left = static_cast<float>(
                last.value(last.frame_count() - 1, 0));
            state.last_right = static_cast<float>(
                last.value(last.frame_count() - 1, 1));
        }
        for (auto const region : event_inputs) {
            region.for_each([&](iv::TimedEvent const& event) {
                if (state.event_count == 0) {
                    state.first_event_time = event.time.bits();
                }
                ++state.event_count;
            });
        }

        for (auto run : sample_outputs.prepared_runs())
            for (std::size_t slot = 0; slot < run.size(); ++slot)
                for (std::size_t channel = 0; channel < 2; ++channel)
                    run.value(slot, channel) = sample_inputs[run.region_index()].at(run.position(slot), channel);
        for (std::size_t region = 0; region < event_inputs.size(); ++region) {
            event_inputs[region].for_each(
                [&](iv::TimedEvent const& event) {
                    if (ctx.output<"observed-event-coverage">().requested_coverage().contains(event.time)) event_outputs[region].write(event);
                });
        }
    }
};

void persisted_recording_coverage_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.persisted_stereo_recording_source">();
    auto consumer = graph.node<
        "iv.test.graph_jit.state_context.persisted_recording_coverage_consumer">();
    consumer(source);
    consumer.connect_event_input(0, source.event_port());

    auto sample_source = graph.node<
        "iv.test.graph_jit.state_context.multi_region_source">();
    auto event_source = graph.node<
        "iv.test.graph_jit.state_context.multi_region_event_source">();
    auto child = graph.node<
        "iv.test.graph_jit.state_context.multi_region_boundary_child_module">();
    child(sample_source);
    child.connect_event_input(0, event_source.event_port());
    auto observer = graph.node<
        "iv.test.graph_jit.state_context.module_boundary_coverage_observer">();
    observer(child);
    observer.connect_event_input(0, child.event_port());
    graph.outputs();
}

void playback_sample_history_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.playback_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_history_consumer">();
    sink(source);
    graph.outputs();
}

// This replacement has the same node and connection shape as
// playback_sample_history_module, but its module definition gives the
// ConfiguredGraph a different identity. The event replacement below does the
// same for playback_event_history_module. They exercise reuse of the same
// generation-local bundle/port coordinates by different semantic nodes.
void playback_sample_history_replacement_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.playback_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_history_consumer">();
    sink(source);
    graph.outputs();
}

void playback_sample_window_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.playback_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink(source);
    graph.outputs();
}

void playback_mixed_sample_module(iv::GraphBuilder& graph)
{
    auto realtime = graph.node<"iv.test.graph_jit.state_context.large_history_ramp_source">();
    auto background = graph.node<"iv.test.graph_jit.state_context.playback_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_stereo_history_consumer">();
    sink(graph.tile<iv::stereo>(realtime, background));
    graph.outputs();
}

void playback_event_history_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.playback_trigger_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_event_history_consumer">();
    sink.connect_event_input(0, source.event_port());
    graph.outputs();
}

void playback_event_history_replacement_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.playback_trigger_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_event_history_consumer">();
    sink.connect_event_input(0, source.event_port());
    graph.outputs();
}

void playback_mixed_event_module(iv::GraphBuilder& graph)
{
    auto realtime = graph.node<"iv.test.graph_jit.state_context.trigger_event_source">();
    auto background = graph.node<"iv.test.graph_jit.state_context.playback_trigger_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_event_history_consumer">();
    std::array ports{realtime.event_port(), background.event_port()};
    sink.connect_event_input(0, graph.make_event_port(ports));
    graph.outputs();
}

// The consumer fed by the realtime source and the consumer fed by playback
// exchange sources in the rewired successor.
void playback_sample_rewire_module(iv::GraphBuilder& graph)
{
    auto realtime = graph.node<"iv.test.graph_jit.state_context.large_history_ramp_source">();
    auto background = graph.node<"iv.test.graph_jit.state_context.playback_sample_source">();
    auto realtime_fed = graph.node<"iv.test.graph_jit.state_context.playback_history_consumer">();
    auto background_fed = graph.node<"iv.test.graph_jit.state_context.playback_history_peer">();
    realtime_fed(realtime);
    background_fed(background);
    graph.outputs();
}

void playback_event_rewire_module(iv::GraphBuilder& graph)
{
    auto realtime = graph.node<"iv.test.graph_jit.state_context.trigger_event_source">();
    auto background = graph.node<"iv.test.graph_jit.state_context.playback_trigger_source">();
    auto realtime_fed = graph.node<"iv.test.graph_jit.state_context.playback_event_history_consumer">();
    auto background_fed = graph.node<"iv.test.graph_jit.state_context.playback_event_history_peer">();
    realtime_fed.connect_event_input(0, realtime.event_port());
    background_fed.connect_event_input(0, background.event_port());
    graph.outputs();
}

// Authors a latent Tick sample output whose newest values stay provisional
// until its next tick revises them through OutputPort::update(). Before
// authoring, it checks every value of its owned window, the last
// history + latency values before this tick, against the value it authored or
// revised for that position. The newest min(latency, block size) values of
// each block are provisional, so the fixture runs equal-sized blocks only.
template<class State, class Output>
void author_latent_window(
    State& state,
    Output const& output,
    std::int64_t begin,
    std::size_t block_size)
{
    auto const final_value = [](std::int64_t position) {
        return static_cast<float>(position);
    };
    auto const provisional_value = [](std::int64_t position) {
        return -1.0f - static_cast<float>(position);
    };
    auto const window = output.history() + output.latency();
    auto const pending = std::min(output.latency(), block_size);
    ++state.calls;
    state.checked = 0;
    state.mismatches = 0;
    for (std::size_t offset = 0; offset < window; ++offset) {
        auto const position = begin - 1 - static_cast<std::int64_t>(offset);
        if (position < 0) break;
        auto const expected = offset < pending
            ? provisional_value(position)
            : final_value(position);
        ++state.checked;
        if (static_cast<float>(output.get(offset)) != expected) {
            ++state.mismatches;
        }
    }
    for (std::size_t offset = 0; offset < pending; ++offset) {
        auto const position = begin - 1 - static_cast<std::int64_t>(offset);
        if (position < 0) break;
        output.update(iv::Sample{final_value(position)}, offset);
    }
    for (std::size_t offset = 0; offset < block_size; ++offset) {
        auto const position = begin + static_cast<std::int64_t>(offset);
        output.push(iv::Sample{offset + pending >= block_size
            ? provisional_value(position)
            : final_value(position)});
    }
}

struct LatentWindowSource {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t checked = 0;
        std::uint64_t mismatches = 0;
    };

    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<LatentWindowSource>& ctx) const
    {
        ctx.output<"out">().history() = 3;
        ctx.output<"out">().latency() = 2;
    }

    void tick_block(iv::TickBlockContext<LatentWindowSource> const& ctx) const
    {
        author_latent_window(
            ctx.state(),
            ctx.output<"out">(),
            ctx.index.whole().signed_value(),
            ctx.block_size);
    }
};
