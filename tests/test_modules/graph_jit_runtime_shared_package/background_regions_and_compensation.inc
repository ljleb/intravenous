struct MultiRegionCoverageSource {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "multi-region-source-output",
            iv::OutputProduction::coverage)};
    }

    static constexpr auto regions()
    {
        return std::array{
            iv::GlobalIndexRange::from_begin_and_end(
                iv::GlobalIndex::from_signed(10),
                iv::GlobalIndex::from_signed(14)),
            iv::GlobalIndexRange::from_begin_and_end(
                iv::GlobalIndex::from_signed(12),
                iv::GlobalIndex::from_signed(16)),
        };
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<MultiRegionCoverageSource>& ctx) const
    {
        auto const published = regions();
        auto output = ctx.output<"multi-region-source-output">();
        output.publish(published);
        for (auto const region : published) output.change(region);
    }

    void tock_coverage(
        iv::TockCoverageContext<MultiRegionCoverageSource>& ctx) const
    {
        for (auto run : ctx.output<"multi-region-source-output">().prepared_runs())
            for (std::size_t slot = 0; slot < run.size(); ++slot)
                run.value(slot) = iv::Sample{run.region_range().begin == regions()[0].begin ? 1.0f : 2.0f};
    }
};

struct MultiRegionCoverageConsumer {
    struct alignas(64) State {
        std::uint64_t calls = 0;
        std::uint64_t region_count = 0;
        std::uint64_t first_begin = 0;
        std::uint64_t second_begin = 0;
        float first_overlap_value = 0.0f;
        float second_overlap_value = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>(
            "multi-region-coverage-input",
            iv::InputAccess::coverage)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "multi-region-consumer-output",
            iv::OutputProduction::coverage)};
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<MultiRegionCoverageConsumer>& ctx) const
    {
        auto input = ctx.input<"multi-region-coverage-input">();
        auto output = ctx.output<"multi-region-consumer-output">();
        output.publish(input.regions());
        output.change(input.changed());
    }

    void propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<MultiRegionCoverageConsumer>& ctx) const
    {
        for (auto region : ctx.input<"multi-region-coverage-input">().coverage()) ctx.input<"multi-region-coverage-input">().require(region);
    }

    void tock_coverage(
        iv::TockCoverageContext<MultiRegionCoverageConsumer>& ctx) const
    {
        auto input = ctx.input<"multi-region-coverage-input">().regions();
        IV_ASSERT(input.size() == 2,
            "multi-Region consumer lost its explicitly requested inspection inputs");

        auto& state = ctx.state();
        ++state.calls;
        state.region_count = input.size();
        state.first_begin = input[0].range().begin.bits();
        state.second_begin = input[1].range().begin.bits();
        auto const overlap = iv::GlobalIndex::from_signed(12);
        state.first_overlap_value = static_cast<float>(input[0].at(overlap));
        state.second_overlap_value = static_cast<float>(input[1].at(overlap));

        for (auto run : ctx.output<"multi-region-consumer-output">().prepared_runs()) {
            auto const source = std::ranges::find_if(input, [&](auto region) {
                return region.range() == run.region_range();
            });
            IV_ASSERT(source != input.end(), "multi-Region output lost its source Region");
            for (std::size_t slot = 0; slot < run.size(); ++slot)
                run.value(slot) = (*source).at(run.position(slot));
        }
    }
};

struct MultiRegionSequentialConsumer {
    std::uint8_t configuration_marker = 0;

    struct alignas(64) State {
        std::uint64_t calls = 0;
        std::uint64_t sample_count = 0;
        std::array<float, 6> values{};
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>(
            "multi-region-sequential-input",
            iv::InputAccess::sequential)};
    }

    static constexpr auto outputs() { return std::tuple{}; }

    void tick_block(
        iv::TickBlockContext<MultiRegionSequentialConsumer> const& ctx) const
    {
        auto const input = ctx.input<"multi-region-sequential-input">().block();
        auto& state = ctx.state();
        ++state.calls;
        state.sample_count = input.size();
        auto const count = std::min(input.size(), state.values.size());
        for (std::size_t i = 0; i < count; ++i) {
            state.values[i] = static_cast<float>(input[i]);
        }
    }
};

struct MultiRegionEventCoverageSource {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            "multi-region-event-source-output",
            iv::EventTypeId::trigger,
            iv::OutputProduction::coverage,
            iv::EventOutputOptions{.max_events_per_index = 1.0})};
    }

    static constexpr auto regions()
    {
        return std::array{
            iv::GlobalIndexRange::from_begin_and_end(
                iv::GlobalIndex::from_signed(10),
                iv::GlobalIndex::from_signed(14)),
            iv::GlobalIndexRange::from_begin_and_end(
                iv::GlobalIndex::from_signed(12),
                iv::GlobalIndex::from_signed(16)),
        };
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<
            MultiRegionEventCoverageSource>& ctx) const
    {
        auto const published = regions();
        auto output = ctx.output<"multi-region-event-source-output">();
        output.publish(published);
        for (auto const region : published) output.change(region);
    }

    void tock_coverage(
        iv::TockCoverageContext<MultiRegionEventCoverageSource>& ctx) const
    {
        auto output =
            ctx.output<"multi-region-event-source-output">().regions();
        auto const& requested = ctx.output<"multi-region-event-source-output">().requested_coverage();
        for (auto region : output) {
            auto const first = region.range().begin.signed_value();
            for (auto const time : {first, first + 3})
                if (requested.contains(iv::GlobalIndex::from_signed(time))) region.write({
                    .time = iv::GlobalIndex::from_signed(time), .value = iv::TriggerEvent{}});
        }
    }
};

struct MultiRegionEventCoverageConsumer {
    struct alignas(64) State {
        std::uint64_t calls = 0;
        std::uint64_t region_count = 0;
        std::uint64_t first_event_count = 0;
        std::uint64_t second_event_count = 0;
        std::uint64_t first_last_time = 0;
        std::uint64_t second_first_time = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::event_input(
            "multi-region-event-coverage-input",
            iv::EventTypeId::trigger,
            iv::InputAccess::coverage)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            "multi-region-event-consumer-output",
            iv::EventTypeId::trigger,
            iv::OutputProduction::coverage,
            iv::EventOutputOptions{.max_events_per_index = 1.0})};
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<
            MultiRegionEventCoverageConsumer>& ctx) const
    {
        auto input = ctx.input<"multi-region-event-coverage-input">();
        auto output = ctx.output<"multi-region-event-consumer-output">();
        output.publish(input.regions());
        output.change(input.changed());
    }

    void propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<
            MultiRegionEventCoverageConsumer>& ctx) const
    {
        for (auto region : ctx.input<"multi-region-event-coverage-input">().coverage()) ctx.input<"multi-region-event-coverage-input">().require(region);
    }

    void tock_coverage(
        iv::TockCoverageContext<MultiRegionEventCoverageConsumer>& ctx) const
    {
        auto input =
            ctx.input<"multi-region-event-coverage-input">().regions();
        auto output =
            ctx.output<"multi-region-event-consumer-output">().regions();
        IV_ASSERT(input.size() == 2,
            "multi-Region event consumer lost its explicitly requested inspection inputs");

        auto& state = ctx.state();
        ++state.calls;
        state.region_count = input.size();
        for (std::size_t region = 0; region < input.size(); ++region) {
            auto const destination = std::ranges::find_if(output, [&](auto value) {
                return value.range() == input[region].range();
            });
            input[region].for_each([&](iv::TimedEvent const& event) {
                if (region == 0) {
                    ++state.first_event_count;
                    state.first_last_time = event.time.bits();
                } else {
                    if (state.second_event_count == 0) {
                        state.second_first_time = event.time.bits();
                    }
                    ++state.second_event_count;
                }
                if (destination != output.end()
                    && ctx.output<"multi-region-event-consumer-output">().requested_coverage().contains(event.time))
                    (*destination).write(event);
            });
        }
    }
};

struct MultiRegionSequentialEventConsumer {
    std::uint8_t configuration_marker = 0;

    struct alignas(128) State {
        std::uint64_t calls = 0;
        std::uint64_t event_count = 0;
        std::array<std::uint64_t, 5> times{};
    };

    static constexpr auto inputs()
    {
        return std::array{iv::event_input(
            "multi-region-sequential-event-input",
            iv::EventTypeId::trigger,
            iv::InputAccess::sequential)};
    }

    static constexpr auto outputs() { return std::tuple{}; }

    void tick_block(
        iv::TickBlockContext<MultiRegionSequentialEventConsumer> const& ctx)
        const
    {
        auto const events =
            ctx.input<"multi-region-sequential-event-input">().events();
        auto& state = ctx.state();
        ++state.calls;
        state.event_count = events.size();
        auto const count = std::min(events.size(), state.times.size());
        for (std::size_t event = 0; event < count; ++event) {
            state.times[event] = events[event].time.bits();
        }
    }
};

struct SingleRegionSampleSource {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "single-region-sample-source-output",
            iv::OutputProduction::region)};
    }

    static constexpr auto region()
    {
        return iv::GlobalIndexRange::from_begin_and_end(
            iv::GlobalIndex::from_signed(20),
            iv::GlobalIndex::from_signed(24));
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<SingleRegionSampleSource>& ctx) const
    {
        auto output = ctx.output<"single-region-sample-source-output">();
        output.publish(region());
        output.change(region());
    }

    void tock_coverage(
        iv::TockCoverageContext<SingleRegionSampleSource>& ctx) const
    {
        auto output = ctx.output<"single-region-sample-source-output">();
        IV_ASSERT(output.region().range() == region(),
            "single sample Region source received the wrong output range");
        for (auto run : output.prepared_runs())
            for (std::size_t slot = 0; slot < run.size(); ++slot)
                run.value(slot) = iv::Sample{static_cast<float>(run.index(slot) + 4)};
    }
};

struct SingleRegionSampleConsumer {
    struct alignas(64) State {
        std::uint64_t calls = 0;
        std::uint64_t begin = 0;
        std::uint64_t extent = 0;
        float first_value = 0.0f;
        float last_value = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>(
            "single-region-sample-input", iv::InputAccess::region)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "single-region-sample-consumer-output",
            iv::OutputProduction::region)};
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<SingleRegionSampleConsumer>& ctx) const
    {
        auto input = ctx.input<"single-region-sample-input">();
        auto const regions = input.regions();
        IV_ASSERT(regions.size() == 1,
            "single sample Region input did not propagate one range");
        auto output = ctx.output<"single-region-sample-consumer-output">();
        output.publish(regions.front());
        output.change(input.changed());
    }

    void propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<SingleRegionSampleConsumer>& ctx) const
    {
        for (auto region : ctx.input<"single-region-sample-input">().coverage()) ctx.input<"single-region-sample-input">().require(region);
    }

    void tock_coverage(
        iv::TockCoverageContext<SingleRegionSampleConsumer>& ctx) const
    {
        auto input = ctx.input<"single-region-sample-input">().region();
        auto output = ctx.output<"single-region-sample-consumer-output">();
        IV_ASSERT(input.range() == output.region().range(),
            "single sample Region input and output ranges differ");
        auto& state = ctx.state();
        ++state.calls;
        state.begin = input.range().begin.bits();
        state.extent = input.range().extent;
        state.first_value = static_cast<float>(input.value(0));
        state.last_value = static_cast<float>(
            input.value(input.frame_count() - 1));
        for (auto run : output.prepared_runs())
            for (std::size_t slot = 0; slot < run.size(); ++slot)
                run.value(slot) = input.at(run.position(slot));
    }
};

struct SingleRegionEventSource {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            "single-region-event-source-output",
            iv::EventTypeId::trigger,
            iv::OutputProduction::region,
            iv::EventOutputOptions{.max_events_per_index = 1.0})};
    }

    static constexpr auto region()
    {
        return iv::GlobalIndexRange::from_begin_and_end(
            iv::GlobalIndex::from_signed(30),
            iv::GlobalIndex::from_signed(34));
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<SingleRegionEventSource>& ctx) const
    {
        auto output = ctx.output<"single-region-event-source-output">();
        output.publish(region());
        output.change(region());
    }

    void tock_coverage(
        iv::TockCoverageContext<SingleRegionEventSource>& ctx) const
    {
        auto output = ctx.output<"single-region-event-source-output">().region();
        IV_ASSERT(output.range() == region(),
            "single event Region source received the wrong output range");
        if (ctx.output<"single-region-event-source-output">().requested_coverage().contains(iv::GlobalIndex::from_signed(30))) output.write({
            .time = iv::GlobalIndex::from_signed(30),
            .value = iv::TriggerEvent{},
        });
        if (ctx.output<"single-region-event-source-output">().requested_coverage().contains(iv::GlobalIndex::from_signed(33))) output.write({
            .time = iv::GlobalIndex::from_signed(33),
            .value = iv::TriggerEvent{},
        });
    }
};

struct SingleRegionEventConsumer {
    struct alignas(64) State {
        std::uint64_t calls = 0;
        std::uint64_t begin = 0;
        std::uint64_t extent = 0;
        std::uint64_t event_count = 0;
        std::uint64_t first_time = 0;
        std::uint64_t last_time = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::event_input(
            "single-region-event-input",
            iv::EventTypeId::trigger,
            iv::InputAccess::region)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            "single-region-event-consumer-output",
            iv::EventTypeId::trigger,
            iv::OutputProduction::region,
            iv::EventOutputOptions{.max_events_per_index = 1.0})};
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<SingleRegionEventConsumer>& ctx) const
    {
        auto input = ctx.input<"single-region-event-input">();
        auto const regions = input.regions();
        IV_ASSERT(regions.size() == 1,
            "single event Region input did not propagate one range");
        auto output = ctx.output<"single-region-event-consumer-output">();
        output.publish(regions.front());
        output.change(input.changed());
    }

    void propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<SingleRegionEventConsumer>& ctx) const
    {
        for (auto region : ctx.input<"single-region-event-input">().coverage()) ctx.input<"single-region-event-input">().require(region);
    }

    void tock_coverage(
        iv::TockCoverageContext<SingleRegionEventConsumer>& ctx) const
    {
        auto input = ctx.input<"single-region-event-input">().region();
        auto output = ctx.output<"single-region-event-consumer-output">().region();
        IV_ASSERT(input.range() == output.range(),
            "single event Region input and output ranges differ");
        auto& state = ctx.state();
        ++state.calls;
        state.begin = input.range().begin.bits();
        state.extent = input.range().extent;
        input.for_each([&](iv::TimedEvent const& event) {
            if (state.event_count == 0) state.first_time = event.time.bits();
            state.last_time = event.time.bits();
            ++state.event_count;
            if (ctx.output<"single-region-event-consumer-output">().requested_coverage().contains(event.time)) output.write(event);
        });
    }
};

void multi_region_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.multi_region_source">();
    auto coverage_consumer = graph.node<
        "iv.test.graph_jit.state_context.multi_region_coverage_consumer">();
    auto sequential_consumer = graph.node<
        "iv.test.graph_jit.state_context.multi_region_sequential_consumer">();
    coverage_consumer(source);
    sequential_consumer(source);
    auto event_source = graph.node<
        "iv.test.graph_jit.state_context.multi_region_event_source">();
    auto event_coverage_consumer = graph.node<
        "iv.test.graph_jit.state_context.multi_region_event_coverage_consumer">();
    auto sequential_event_consumer = graph.node<
        "iv.test.graph_jit.state_context.multi_region_sequential_event_consumer">();
    event_coverage_consumer.connect_event_input(0, event_source.event_port());
    sequential_event_consumer.connect_event_input(
        0, event_source.event_port());
    auto single_sample_source = graph.node<
        "iv.test.graph_jit.state_context.single_region_sample_source">();
    auto single_sample_consumer = graph.node<
        "iv.test.graph_jit.state_context.single_region_sample_consumer">();
    single_sample_consumer(single_sample_source);
    auto single_event_source = graph.node<
        "iv.test.graph_jit.state_context.single_region_event_source">();
    auto single_event_consumer = graph.node<
        "iv.test.graph_jit.state_context.single_region_event_consumer">();
    single_event_consumer.connect_event_input(
        0, single_event_source.event_port());
    graph.outputs();
}

void native_batch_tick_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.native_batch_tick">(
        std::uint64_t{11});
    (void)graph.node<"iv.test.graph_jit.state_context.native_batch_tick">(
        std::uint64_t{22});
    graph.outputs();
}

void native_batch_aliasing_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.native_batch_aliasing">(
        std::uint64_t{11});
    (void)graph.node<"iv.test.graph_jit.state_context.native_batch_aliasing">(
        std::uint64_t{22});
    graph.outputs();
}

void native_batch_replay_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.native_batch_replay">(
        std::uint64_t{11});
    auto consumer = graph.node<
        "iv.test.graph_jit.state_context.native_batch_background">(
        std::uint64_t{22});
    consumer(source);
    graph.outputs();
}

void native_batch_activity_module(iv::GraphBuilder& graph)
{
    auto first = graph.node<
        "iv.test.graph_jit.state_context.native_batch_activity">();
    first.ttl(64);
    auto second = graph.node<
        "iv.test.graph_jit.state_context.native_batch_activity">();
    second.ttl(128);
    // Keeping both outputs live until one consumer prevents transient
    // storage sharing from forcing scalar fallback.
    auto sink = graph.node<
        "iv.test.graph_jit.state_context.native_batch_activity_sink">();
    sink(first, second);
    graph.outputs();
}

void native_batch_background_module(iv::GraphBuilder& graph)
{
    // These probes have no inputs, so this module isolates forward propagation
    // and Tock batching from Random Access input coverage.
    (void)graph.node<
        "iv.test.graph_jit.state_context.native_batch_tock">(
        std::uint64_t{11});
    (void)graph.node<
        "iv.test.graph_jit.state_context.native_batch_tock">(
        std::uint64_t{22});
    graph.outputs();
}

void native_batch_reverse_module(iv::GraphBuilder& graph)
{
    auto first_source = graph.node<
        "iv.test.graph_jit.state_context.background_evaluation_call">();
    auto second_source = graph.node<
        "iv.test.graph_jit.state_context.background_evaluation_call">();
    auto first = graph.node<
        "iv.test.graph_jit.state_context.native_batch_background">(
        std::uint64_t{11});
    auto second = graph.node<
        "iv.test.graph_jit.state_context.native_batch_background">(
        std::uint64_t{22});
    first(first_source);
    second(second_source);
    graph.outputs();
}

} // namespace

IV_NODE("iv.test.graph_jit.state_context.native_batch_tick", NativeBatchTickProbe, "Native batch tick");
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_aliasing",
    NativeBatchAliasingProbe, "Native batch aliasing");
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_replay",
    NativeBatchReplaySource, "Native batch replay");
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_background",
    NativeBatchBackgroundProbe, "Native batch background");
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_tock",
    NativeBatchTockProbe, "Native batch tock");
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_activity",
    NativeBatchActivityProbe, "Native batch activity");
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_activity_sink",
    NativeBatchActivitySink, "Native batch activity sink");
IV_NODE("iv.test.graph_jit.state_context.activity_step_source", ActivityStepSource, "Activity step source");
IV_NODE("iv.test.graph_jit.state_context.activity_probe", ActivityProbe, "Activity probe");
IV_NODE("iv.test.graph_jit.state_context.activity_type_default_probe", ActivityTypeDefaultProbe, "Activity type default probe");
IV_NODE("iv.test.graph_jit.state_context.event_activity_step_source", EventActivityStepSource, "Event activity step source");
IV_NODE("iv.test.graph_jit.state_context.event_activity_probe", EventActivityProbe, "Event activity probe");
IV_NODE("iv.test.graph_jit.state_context.declaration_layout_parent", DeclarationLayoutParent, "Declaration layout parent");
IV_NODE("iv.test.graph_jit.state_context.declaration_layout_producer", DeclarationLayoutProducer, "Declaration layout producer");
IV_NODE("iv.test.graph_jit.state_context.declaration_layout_consumer", DeclarationLayoutConsumer, "Declaration layout consumer");
IV_NODE("iv.test.graph_jit.state_context.fft_value_producer", FFTValueProducer, "FFT value producer");
IV_NODE("iv.test.graph_jit.state_context.fft_value_consumer", FFTValueConsumer, "FFT value consumer");
IV_NODE("iv.test.graph_jit.state_context.fractional_index_producer", FractionalIndexProducer, "Fractional index producer");
IV_NODE("iv.test.graph_jit.state_context.fractional_index_consumer", FractionalIndexConsumer, "Fractional index consumer");
IV_NODE("iv.test.graph_jit.state_context.persisted_fft_value_producer", PersistedFFTValueProducer, "Persisted FFT value producer");
IV_NODE("iv.test.graph_jit.state_context.persisted_non_project_rate_sample_producer", PersistedNonProjectRateSampleProducer, "Persisted non project rate sample producer");
IV_NODE(
    "iv.test.graph_jit.state_context.multi_region_source",
    MultiRegionCoverageSource, "Multi region source");
IV_NODE(
    "iv.test.graph_jit.state_context.multi_region_coverage_consumer",
    MultiRegionCoverageConsumer, "Multi region coverage consumer");
IV_NODE(
    "iv.test.graph_jit.state_context.multi_region_sequential_consumer",
    MultiRegionSequentialConsumer, "Multi region sequential consumer");
IV_NODE(
    "iv.test.graph_jit.state_context.multi_region_event_source",
    MultiRegionEventCoverageSource, "Multi region event source");
IV_NODE(
    "iv.test.graph_jit.state_context.multi_region_event_coverage_consumer",
    MultiRegionEventCoverageConsumer, "Multi region event coverage consumer");
IV_NODE(
    "iv.test.graph_jit.state_context.multi_region_sequential_event_consumer",
    MultiRegionSequentialEventConsumer, "Multi region sequential event consumer");
IV_NODE(
    "iv.test.graph_jit.state_context.single_region_sample_source",
    SingleRegionSampleSource, "Single region sample source");
IV_NODE(
    "iv.test.graph_jit.state_context.single_region_sample_consumer",
    SingleRegionSampleConsumer, "Single region sample consumer");
IV_NODE(
    "iv.test.graph_jit.state_context.single_region_event_source",
    SingleRegionEventSource, "Single region event source");
IV_NODE(
    "iv.test.graph_jit.state_context.single_region_event_consumer",
    SingleRegionEventConsumer, "Single region event consumer");
IV_MODULE("iv.test.graph_jit.state_context.activity_module", activity_module, "Activity module");
IV_MODULE("iv.test.graph_jit.state_context.activity_disabled_module", activity_disabled_module, "Activity disabled module");
IV_MODULE("iv.test.graph_jit.state_context.activity_type_default_module", activity_type_default_module, "Activity type default module");
IV_MODULE("iv.test.graph_jit.state_context.event_activity_module", event_activity_module, "Event activity module");
IV_MODULE("iv.test.graph_jit.state_context.declaration_layout_module", declaration_layout_module, "Declaration layout module");
IV_MODULE("iv.test.graph_jit.state_context.fft_value_module", fft_value_module, "FFT value module");
IV_MODULE("iv.test.graph_jit.state_context.fractional_index_value_module", fractional_index_value_module, "Fractional index value module");
IV_MODULE("iv.test.graph_jit.state_context.persisted_fft_value_module", persisted_fft_value_module, "Persisted FFT value module");
IV_MODULE("iv.test.graph_jit.state_context.persisted_non_project_rate_sample_module", persisted_non_project_rate_sample_module, "Persisted non project rate sample module");
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_tick_module",
    native_batch_tick_module, "Native batch tick module");
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_aliasing_module",
    native_batch_aliasing_module, "Native batch aliasing module");
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_replay_module",
    native_batch_replay_module, "Native batch replay module");
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_background_module",
    native_batch_background_module, "Native batch background module");
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_activity_module",
    native_batch_activity_module, "Native batch activity module");
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_reverse_module",
    native_batch_reverse_module, "Native batch reverse module");
IV_MODULE(
    "iv.test.graph_jit.state_context.multi_region_module",
    multi_region_module, "Multi region module");

namespace {
// stereo_ramp_source offset by 100000.
struct OffsetStereoRampSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential,
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    void tick_block(iv::TickBlockContext<OffsetStereoRampSource> const& ctx) const
    {
        auto output = ctx.output<"out">();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto const position = static_cast<float>(
                ctx.index.advanced_by({.whole = i}).whole().signed_value());
            output.value(i, 0) = iv::Sample{position + 100000.0f};
            output.value(i, 1) = iv::Sample{position + 101000.0f};
        }
    }
};

// The probe's interleaved mono input downmixes the stereo ramp, compensated by
// 7 for its slower input, which reads the delay node. `rewired` downmixes the
// offset stereo ramp instead.
void downmixed_rewire_module(iv::GraphBuilder& graph, bool rewired = false)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.stereo_ramp_source">();
    auto offset_source =
        graph.node<"iv.test.graph_jit.state_context.offset_stereo_ramp_source">();
    auto ramp = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.converted_sequence_probe">();
    delayed(ramp);
    auto const in = rewired
        ? static_cast<iv::SamplePortRef>(offset_source)
        : static_cast<iv::SamplePortRef>(source);
    probe(in, delayed);
    graph.outputs();
}

// The probe's interleaved mono input downmixes a stereo tile of the ramp and a
// second ramp, or of the ramp and the offset ramp when `rewired`, compensated
// by 7 for its slower input, which reads the delay node.
void downmixed_tile_rewire_module(iv::GraphBuilder& graph, bool rewired = false)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto other = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto offset_source =
        graph.node<"iv.test.graph_jit.state_context.offset_sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.converted_sequence_probe">();
    delayed(source);
    auto const right = rewired
        ? static_cast<iv::SamplePortRef>(offset_source)
        : static_cast<iv::SamplePortRef>(other);
    probe(graph.tile<iv::stereo>(source, right), delayed);
    graph.outputs();
}

// The probe's interleaved mono input downmixes the stereo ramp. Its slower
// input reads a ramp, or the delay node when `delayed_slower`, which
// compensates the downmixed input by 7. A history reader keeps the stereo
// ramp's recent values.
void downmixed_compensation_module(
    iv::GraphBuilder& graph,
    bool delayed_slower = false)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.stereo_ramp_source">();
    auto ramp = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto history_reader =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.converted_sequence_probe">();
    delayed(ramp);
    history_reader(source);
    auto const slower = delayed_slower
        ? static_cast<iv::SamplePortRef>(delayed)
        : static_cast<iv::SamplePortRef>(ramp);
    probe(source, slower);
    graph.outputs();
}

// The probe's interleaved mono input downmixes a stereo tile of the ramp and a
// second ramp. Its slower input reads the second ramp, or the delay node when
// `delayed_slower`, which compensates the downmixed input by 7. History
// readers keep both ramps' recent values.
void downmixed_tile_compensation_module(
    iv::GraphBuilder& graph,
    bool delayed_slower = false)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto other = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto source_history =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    auto other_history =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.converted_sequence_probe">();
    delayed(source);
    source_history(source);
    other_history(other);
    auto const slower = delayed_slower
        ? static_cast<iv::SamplePortRef>(delayed)
        : static_cast<iv::SamplePortRef>(other);
    probe(graph.tile<iv::stereo>(source, other), slower);
    graph.outputs();
}

// neutral_latency_module, with a history reader that keeps the ramp's recent
// values in storage.
void retained_neutral_latency_module(
    iv::GraphBuilder& graph,
    std::size_t latency = 0)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.latency_argument_ramp_source">(latency);
    auto history_reader =
        graph.node<"iv.test.graph_jit.state_context.history_consumer">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.neutral_sequence_probe">();
    history_reader(source);
    probe(source);
    graph.outputs();
}
} // namespace

IV_NODE(
    "iv.test.graph_jit.state_context.offset_stereo_ramp_source",
    OffsetStereoRampSource, "Offset stereo ramp source");
IV_MODULE(
    "iv.test.graph_jit.state_context.downmixed_rewire_module",
    downmixed_rewire_module, "Downmixed rewire module");
IV_MODULE(
    "iv.test.graph_jit.state_context.downmixed_tile_rewire_module",
    downmixed_tile_rewire_module, "Downmixed tile rewire module");
IV_MODULE(
    "iv.test.graph_jit.state_context.downmixed_compensation_module",
    downmixed_compensation_module, "Downmixed compensation module");
IV_MODULE(
    "iv.test.graph_jit.state_context.downmixed_tile_compensation_module",
    downmixed_tile_compensation_module, "Downmixed tile compensation module");
IV_MODULE(
    "iv.test.graph_jit.state_context.retained_neutral_latency_module",
    retained_neutral_latency_module, "Retained neutral latency module");
