struct SuspendableCoverageProbe {
    struct State { std::uint64_t calls = 0; float first = 0; };
    static constexpr auto inputs() {
        return std::array{iv::input<iv::Sample>({"in", "In"}, iv::InputAccess::region)};
    }
    static constexpr auto outputs() {
        return std::array{iv::output<iv::Sample>({"suspendable_output", "Suspendable output"},
            iv::OutputProduction::region, iv::OutputRealization::buffered)};
    }
    iv::CoverageTask propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<SuspendableCoverageProbe>& ctx) const {
        auto const& metadata = co_await ctx.request_input_coverage<"in">();
        std::vector<iv::SamplePositionRun> requested;
        for (auto const range : metadata.coverage) requested.push_back({range.begin, range.extent});
        auto input = co_await ctx.request_input<"in">(iv::SamplePositionsView{.runs = requested});
        auto const begin = iv::GlobalIndex::from_signed(
            static_cast<std::int64_t>(input.region().value(0).value));
        auto output = ctx.output<"suspendable_output">();
        auto const region = iv::GlobalIndexRange{begin, input.region().range().extent};
        output.publish(region);
        output.change(region);
    }
    iv::CoverageTask propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<SuspendableCoverageProbe>& ctx) const {
        auto const& metadata = co_await ctx.request_input_coverage<"in">();
        std::vector<iv::SamplePositionRun> requested;
        for (auto const range : metadata.coverage) requested.push_back({range.begin, range.extent});
        auto input = co_await ctx.request_input<"in">(iv::SamplePositionsView{.runs = requested});
        ctx.input<"in">().require(input.region().range());
    }
    void tock_coverage(iv::TockCoverageContext<SuspendableCoverageProbe>& ctx) const {
        auto input = ctx.input<"in">().region();
        ++ctx.state().calls;
        ctx.state().first = input.value(0).value;
        for (auto run : ctx.output<"suspendable_output">().prepared_runs())
            for (std::size_t slot = 0; slot < run.size(); ++slot)
                run.value(slot) = input.value(static_cast<std::size_t>(run.index(slot)));
    }
};
IV_NODE("iv.test.graph_jit.suspendable_coverage_probe", SuspendableCoverageProbe, "Suspendable coverage probe");
void suspendable_coverage_module(iv::GraphBuilder& graph) {
    auto source = graph.node<"iv.test.graph_jit.state_context.single_region_sample_source">();
    auto probe = graph.node<"iv.test.graph_jit.suspendable_coverage_probe">();
    probe(source);
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.suspendable_coverage_module", suspendable_coverage_module, "Suspendable coverage module");

// Deliberately private: no IV_NODE registration for this concrete type.
struct PrivateArithmeticProbe {
    struct State {
        std::uint64_t calls = 0;
        iv::Sample first = 0;
        iv::Sample last = 0;
    };
    static constexpr auto inputs() {
        return std::array{iv::input<iv::Sample>({"in", "In"})};
    }
    static constexpr auto outputs() { return std::tuple{}; }
    void tick_block(iv::TickBlockContext<PrivateArithmeticProbe> const& ctx) const {
        auto& state = ctx.state();
        auto input = ctx.input<"in">();
        ++state.calls;
        state.first = input.get_value(0);
        state.last = input.get_value(ctx.block_size - 1);
    }
};
void private_arithmetic_module(iv::GraphBuilder& graph) {
    auto ramp = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto probe = iv::details::configure_concrete_node<PrivateArithmeticProbe>(graph);
    // This fixture loads its own package, without the shipped builtin package.
    // Use its registered constants; arithmetic still creates internal leaves.
    auto half = graph.node<"iv.test.graph_jit.state_context.constant">(iv::Sample{0.5f});
    auto two = graph.node<"iv.test.graph_jit.state_context.constant">(iv::Sample{2.0f});
    probe(ramp * half + two);
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.private_arithmetic_module", private_arithmetic_module, "Private arithmetic module");
void summed_module_boundaries(iv::GraphBuilder& graph) {
    auto voices = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
        for (std::size_t voice = 0; voice < 17; ++voice) {
            auto source = graph.node<"iv.test.graph_jit.state_context.stereo_ramp_source">();
            boundary.outputs(source);
        }
    }, "summed-voices");
    auto pass = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
        auto in = boundary.input<iv::stereo>({"In"});
        boundary.outputs(in);
    }, "summed-input");
    auto extra = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    pass(voices);
    pass(extra);
    auto probe = iv::details::configure_concrete_node<PrivateArithmeticProbe>(graph);
    probe(pass);
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.summed_module_boundaries", summed_module_boundaries, "Summed module boundaries");

struct HugeComputedRegionSource {
    struct State { std::uint64_t values_written = 0; std::uint64_t logical_extent = 0; };
    static constexpr auto outputs() {
        return std::array{iv::output<iv::Sample>({"huge", "Huge"}, iv::OutputProduction::region)};
    }
    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<HugeComputedRegionSource>& ctx) const {
        iv::GlobalIndexRange const region{iv::GlobalIndex::from_bits(0), std::uint64_t{1} << 50};
        ctx.output<"huge">().publish(region);
        if (ctx.local_state_changed) ctx.output<"huge">().change(region);
    }
    void tock_coverage(iv::TockCoverageContext<HugeComputedRegionSource>& ctx) const {
        auto output = ctx.output<"huge">();
        ctx.state().logical_extent = output.region().range().extent;
        for (auto run : output.prepared_runs()) {
            for (std::size_t slot = 0; slot < run.size(); ++slot) {
                run.value(slot) = iv::Sample{static_cast<float>(run.position(slot).bits() & 1023)};
                ++ctx.state().values_written;
            }
        }
    }
};
IV_NODE("iv.test.computed.huge_source", HugeComputedRegionSource, "Huge source");
void huge_computed_playback(iv::GraphBuilder& graph) {
    auto source = graph.node<"iv.test.computed.huge_source">();
    auto consumer = graph.node<"iv.test.graph_jit.state_context.manual_control_slice_probe">();
    consumer(source);
    graph.outputs();
}
IV_MODULE("iv.test.computed.huge_playback", huge_computed_playback, "Huge playback");

struct CompleteRegionCopy {
    static constexpr auto inputs() {
        return std::array{iv::input<iv::Sample>({"in", "In"}, iv::InputAccess::region,
            iv::InputContentAccess::complete_buffer)};
    }
    static constexpr auto outputs() {
        return std::array{iv::output<iv::Sample>({"out", "Out"}, iv::OutputProduction::region)};
    }
    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<CompleteRegionCopy>& ctx) const {
        auto input = ctx.input<"in">();
        if (!input.regions().empty()) ctx.output<"out">().publish(input.regions()[0]);
        ctx.output<"out">().change(input.changed());
    }
    void propagate_reverse_coverage(iv::PropagateReverseCoverageContext<CompleteRegionCopy>& ctx) const {
        ctx.input<"in">().require(ctx.output<"out">().required_positions());
    }
    void tock_coverage(iv::TockCoverageContext<CompleteRegionCopy>& ctx) const {
        auto input = ctx.input<"in">().region();
        for (auto run : ctx.output<"out">().prepared_runs())
            for (std::size_t slot = 0; slot < run.size(); ++slot)
                run.value(slot) = input.at(run.position(slot));
    }
};
IV_NODE("iv.test.computed.complete_copy", CompleteRegionCopy, "Complete copy");
void invalid_computed_complete_connection(iv::GraphBuilder& graph) {
    auto source = graph.node<"iv.test.computed.huge_source">();
    auto complete = graph.node<"iv.test.computed.complete_copy">();
    complete(source);
    graph.outputs();
}
IV_MODULE("iv.test.computed.invalid_complete_connection", invalid_computed_complete_connection, "Invalid complete connection");
struct BufferedRegionForTick {
    static constexpr auto outputs() {
        return std::array{iv::output<iv::Sample>({"buffered", "Buffered"}, iv::OutputProduction::region, iv::OutputRealization::buffered)};
    }
    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<BufferedRegionForTick>& ctx) const {
        ctx.output<"buffered">().publish({iv::GlobalIndex::from_signed(20), 4});
    }
    void tock_coverage(iv::TockCoverageContext<BufferedRegionForTick>& ctx) const {
        for (auto run : ctx.output<"buffered">().prepared_runs())
            for (std::size_t slot = 0; slot < run.size(); ++slot)
                run.value(slot) = iv::Sample{static_cast<float>(run.position(slot).signed_value())};
    }
};
IV_NODE("iv.test.computed.buffered_for_tick", BufferedRegionForTick, "Buffered for tick");
struct CompleteTickRegionSink {
    static constexpr auto inputs() {
        return std::array{iv::input<iv::Sample>({"complete", "Complete"}, iv::InputAccess::region, iv::InputContentAccess::complete_buffer)};
    }
    void tick_block(iv::TickBlockContext<CompleteTickRegionSink> const& ctx) const {
        auto region = ctx.input<"complete">().region();
        IV_ASSERT(region.has_complete_buffer() && region.frame_count() == 4,
            "complete-buffer Tick input was not prepared");
        IV_ASSERT(static_cast<float>(region.value(0)) == 20.0f
            && static_cast<float>(region.value(3)) == 23.0f,
            "complete-buffer Tick input has incorrect contents");
    }
};
IV_NODE("iv.test.computed.complete_tick_sink", CompleteTickRegionSink, "Complete tick sink");
void complete_tick_input_module(iv::GraphBuilder& graph) {
    auto source = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
        boundary.outputs(graph.node<"iv.test.computed.buffered_for_tick">());
    }, "buffered-region-export");
    auto sink = graph.node<"iv.test.computed.complete_tick_sink">();
    sink(source);
    graph.outputs();
}
IV_MODULE("iv.test.computed.complete_tick_input", complete_tick_input_module, "Complete tick input");

constexpr auto resampler_source_begin = iv::GlobalIndex::from_signed(
    std::numeric_limits<std::int64_t>::max() - 1);

template<int Pace, iv::OutputRealization Realization = iv::OutputRealization::buffered,
    bool RejectTailDemand = false>
struct ResamplerSource {
    static constexpr auto outputs() {
        return std::array{iv::output<iv::Sample>({"out", "Out"},
            iv::OutputProduction::coverage, Realization)};
    }

    void constrain_ports(iv::ConstrainPortsContext<ResamplerSource>& ctx) const {
        ctx.template output<"out">().pace().absolute() = Pace;
    }

    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<ResamplerSource>& ctx) const {
        std::array<iv::GlobalIndexRange, RejectTailDemand ? 3 : 2> regions{};
        regions[0] = {resampler_source_begin, 3};
        regions[1] = {resampler_source_begin.advanced_by(4), 2};
        if constexpr (RejectTailDemand)
            regions[2] = {iv::GlobalIndex::from_signed(100000), 1};
        auto out = ctx.template output<"out">();
        out.publish(regions);
        if (ctx.local_state_changed) for (auto range : regions) out.change(range);
    }

    void tock_coverage(iv::TockCoverageContext<ResamplerSource>& ctx) const {
        for (auto run : ctx.template output<"out">().prepared_runs()) {
            for (std::size_t slot = 0; slot < run.size(); ++slot) {
                if constexpr (RejectTailDemand)
                    IV_ASSERT(run.position(slot) != iv::GlobalIndex::from_signed(100000),
                        "replay evaluated taps due after its final callback");
                auto const offset = resampler_source_begin.forward_distance_to(run.position(slot));
                auto const value = 2.f * static_cast<float>(offset + 1);
                run.value(slot, 0) = iv::Sample{value};
            }
        }
    }
};

template<int Pace, int SourcePace, bool Tail = false>
struct ResamplerPositions {
    static constexpr auto replay_mode = iv::TickReplayMode::enabled;

    static constexpr auto outputs() {
        return iv::TransportPosition::outputs();
    }

    void constrain_ports(iv::ConstrainPortsContext<ResamplerPositions>& ctx) const {
        ctx.template output<"position">().pace().absolute() = Pace;
    }

    void tick(iv::TickSampleContext<ResamplerPositions> const& ctx) const {
        if constexpr (Tail) {
            if (ctx.value_index >= 8) {
                ctx.template output<"position">().push(iv::FractionalGlobalIndex::from_global(
                    iv::GlobalIndex::from_signed(100000)));
                return;
            }
        }
        constexpr std::array<std::uint64_t, 8> whole_offsets{4, 1, 4, 2, 3, 6, 1, 4};
        auto const slot = ctx.value_index % whole_offsets.size();
        auto fraction = slot < 5 ? std::uint64_t{1} << 63 : 0;
        auto whole = resampler_source_begin.advanced_by(whole_offsets[slot]).bits();
        if constexpr (SourcePace == 2) {
            fraction = (whole << 63) | (fraction >> 1);
            whole >>= 1;
        }
        ctx.template output<"position">().push(iv::FractionalGlobalIndex::from_parts(
            iv::GlobalIndex::from_bits(whole), fraction));
    }
};

struct ResamplerProbe {
    static constexpr auto inputs() {
        return std::array{iv::input<iv::Sample>({"in", "In"},
            iv::InputAccess::sequential)};
    }

    void tick(iv::TickSampleContext<ResamplerProbe> const& ctx) const {
        constexpr std::array expected{11.f, 5.f, 11.f, 3.f, 5.f, 0.f, 4.f, 10.f};
        auto const value = expected[ctx.value_index % expected.size()];
        auto const in = ctx.input<"in">();
        IV_ASSERT(in.get().value == value,
            "resampler must deliver the addressed samples at the current output position");
    }
};

template<int Pace, int SourcePace = 1>
void complete_buffer_resampler_module(iv::GraphBuilder& graph) {
    using namespace iv;
    auto source = iv::details::configure_concrete_node<ResamplerSource<SourcePace>>(graph);
    auto position = iv::details::configure_concrete_node<ResamplerPositions<Pace, SourcePace>>(graph);
    auto resampler = iv::details::configure_concrete_node<iv::LinearResampler>(graph);
    auto probe = iv::details::configure_concrete_node<ResamplerProbe>(graph);
    resampler("position"_P = position, "source"_P = source);
    probe(resampler);
    graph.outputs();
}
IV_MODULE("iv.test.resampler.complete_buffer", complete_buffer_resampler_module<1>, "Complete buffer resampler");
IV_MODULE("iv.test.resampler.fast_complete_buffer", complete_buffer_resampler_module<2>, "Fast complete buffer resampler");
IV_MODULE("iv.test.resampler.fast_source", (complete_buffer_resampler_module<1, 2>), "Fast resampler source");

void replay_complete_buffer_resampler_module(iv::GraphBuilder& graph) {
    using namespace iv;
    auto source = details::configure_concrete_node<ResamplerSource<1>>(graph);
    auto position = details::configure_concrete_node<ResamplerPositions<1, 1>>(graph);
    auto resampler = details::configure_concrete_node<LinearResampler>(graph);
    auto window = graph.node<"iv.test.replay.window">(std::int64_t{0}, std::uint64_t{8});
    resampler("position"_P = position, "source"_P = source);
    window(resampler);
    graph.outputs();
}
IV_MODULE("iv.test.resampler.replay", replay_complete_buffer_resampler_module, "Complete buffer resampler replay");

void replay_computed_resampler_module(iv::GraphBuilder& graph) {
    using namespace iv;
    auto source = details::configure_concrete_node<ResamplerSource<1, OutputRealization::computed, true>>(graph);
    auto position = details::configure_concrete_node<ResamplerPositions<1, 1, true>>(graph);
    auto resampler = details::configure_concrete_node<LinearResampler>(graph);
    auto window = graph.node<"iv.test.replay.window">(std::int64_t{0}, std::uint64_t{8});
    resampler("position"_P = position, "source"_P = source);
    window(resampler);
    graph.outputs();
}
IV_MODULE("iv.test.resampler.computed_replay", replay_computed_resampler_module, "Computed resampler replay");

struct DeferredResamplerProbe {
    struct State { std::size_t samples = 0; };
    static constexpr auto inputs() {
        return std::array{iv::input<iv::Sample>({"in", "In"})};
    }
    void tick(iv::TickSampleContext<DeferredResamplerProbe> const& ctx) const {
        constexpr std::array expected{11.f, 5.f, 11.f, 3.f, 5.f, 0.f, 4.f, 10.f};
        auto const current = ctx.state().samples++;
        if (current < 64) return;
        auto const authored = current - 64;
        auto const position = authored < 64 ? authored : 13 + authored - 64;
        IV_ASSERT(ctx.input<"in">().get().value == expected[position % expected.size()],
            "deferred resampler changed the source position of a pending sample");
    }
};

void deferred_resampler_module(iv::GraphBuilder& graph) {
    using namespace iv;
    auto source = details::configure_concrete_node<ResamplerSource<1, OutputRealization::computed>>(graph);
    auto position = details::configure_concrete_node<ResamplerPositions<1, 1>>(graph);
    auto resampler = details::configure_concrete_node<LinearResampler>(graph);
    auto probe = details::configure_concrete_node<DeferredResamplerProbe>(graph);
    resampler("position"_P = position, "source"_P = source);
    probe(resampler);
    graph.outputs();
}
IV_MODULE("iv.test.resampler.deferred", deferred_resampler_module, "Deferred computed-source resampler");

struct CurvePointTransform {
    static constexpr auto inputs() {
        return std::array{iv::event_input({"points", "Points"}, iv::EventTypeId::curve_point, iv::InputAccess::region)};
    }
    static constexpr auto outputs() {
        return std::array{iv::event_output({"transformed", "Transformed"}, iv::EventTypeId::curve_point,
            iv::OutputProduction::region, iv::OutputRealization::buffered)};
    }
    void constrain_ports(iv::ConstrainPortsContext<CurvePointTransform>& ctx) const {
        ctx.output<"transformed">().max_event_count() = 3;
    }
    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<CurvePointTransform>& ctx) const {
        auto input = ctx.input<"points">();
        auto ranges = input.regions();
        ctx.output<"transformed">().publish(ranges.empty() ? iv::GlobalIndexRange{} : ranges.front());
        ctx.output<"transformed">().change(input.changed());
    }
    void propagate_reverse_coverage(iv::PropagateReverseCoverageContext<CurvePointTransform>& ctx) const {
        auto input = ctx.input<"points">();
        for (auto range : input.coverage()) input.require(range);
    }
    void tock_coverage(iv::TockCoverageContext<CurvePointTransform>& ctx) const {
        auto output = ctx.output<"transformed">().region();
        ctx.input<"points">().region().for_each([&](iv::TimedEvent event) {
            auto& point = std::get<iv::CurvePointEvent>(event.value);
            point.value.value *= 2;
            if (event.time == iv::GlobalIndex::from_signed(4)) point.interpolation = iv::CurveInterpolation::empty;
            if (ctx.output<"transformed">().requested_coverage().contains(event.time)) output.write(event);
        });
    }
};
IV_NODE("iv.test.automation.transform", CurvePointTransform, "Transform");
void transformed_sparse_curve_module(iv::GraphBuilder& graph) {
    using namespace iv;
    auto points = graph.node<iv::automation::PointSource>(graph.immutable_array<iv::automation::Point>(
        std::vector<iv::automation::Point>{{1, 0, 0, iv::CurveInterpolation::bezier, 0},
            {2, 4.0 / 88200, 1, iv::CurveInterpolation::hold, 1},
            {3, 12.0 / 88200, 0, iv::CurveInterpolation::empty, 1}}));
    auto transformed = graph.node<"iv.test.automation.transform">();
    transformed("points"_F = points);
    auto curve = graph.node<iv::automation::Curve>();
    curve("points"_F = transformed);
    auto manual = graph.node<"iv.test.replay.manual_full">();
    manual(curve);
    auto window = graph.node<"iv.test.replay.window">(std::int64_t{0}, std::uint64_t{8});
    window(manual);
    graph.outputs();
}
IV_MODULE("iv.test.automation.transformed_sparse_curve", transformed_sparse_curve_module, "Transformed sparse curve");
void project_beat_grid_module(iv::GraphBuilder& graph) {
    auto beats = graph.node<"builtin.beats">();
    auto timeline = graph.node<"builtin.timeline">();
    timeline.connect_event_input(0, beats.event_port("beats"));
    graph.subgraph([&](iv::SubgraphBuilder& boundary) {
        auto nested_beats = graph.node<"builtin.beats">(240.0, 3u, std::vector<std::uint32_t>{3});
        auto nested_timeline = graph.node<"builtin.timeline">();
        nested_timeline.connect_event_input(0, nested_beats.event_port("beats"));
        boundary.outputs();
    }, "nested");
    graph.outputs();
}
IV_MODULE("iv.test.replay.project_beat_grid", project_beat_grid_module, "Project beat grid");
void empty_sparse_curve_module(iv::GraphBuilder& graph) {
    auto curve = graph.node<"builtin.automation.curve">(std::vector<iv::automation::Point>{});
    auto manual = graph.node<"iv.test.replay.manual_full">();
    manual(curve);
    auto window = graph.node<"iv.test.replay.window">(std::int64_t{0}, std::uint64_t{8});
    window(manual);
    graph.outputs();
}
IV_MODULE("iv.test.automation.empty_sparse_curve", empty_sparse_curve_module, "Empty sparse curve");

void rolling_automation_module(iv::GraphBuilder& graph) {
    auto curve = graph.node<"builtin.automation.curve">(
        std::vector<iv::automation::Point>{{1, 0, 1, iv::CurveInterpolation::bezier, 0},
            {2, 1, 2, iv::CurveInterpolation::bezier, 0}});
    auto module = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
        auto frequency = boundary.input<iv::Sample>({"Frequency"}, iv::InputAccess::sequential, {
            .default_value = .25f});
        auto consumer = graph.node<"iv.test.replay.manual_full">();
        consumer(frequency);
        boundary.outputs(consumer);
    }, "automation-boundary");
    module(curve);
    auto live = graph.node<"iv.test.replay.live_probe">();
    live(module);
    graph.outputs();
}
IV_MODULE("iv.test.automation.rolling", rolling_automation_module, "Rolling");
