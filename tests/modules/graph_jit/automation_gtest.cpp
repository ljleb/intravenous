#include "test_support.h"

#include <intravenous/basic_nodes/automation.h>
#include <chrono>
#include <thread>

namespace iv::test::graph_jit_tests {

TEST_F(GraphJitRuntimeFixture, SparseTockPointsComputeOnlyTheRequestedCurveWindow)
{
    auto compiled = compile("iv.test.automation.window_module", 710);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    auto const& plan = graph.background_evaluation_plan;
    auto find_port = [&](std::string_view name, iv::PortKind kind,
                         iv::graph_jit::PortDirection direction) {
        return std::ranges::find_if(plan.ports, [&](auto const& port) {
            return port.name == name && port.kind == kind && port.direction == direction;
        });
    };
    auto source = find_port("points", iv::PortKind::event, iv::graph_jit::PortDirection::output);
    auto curve = find_port("value", iv::PortKind::sample, iv::graph_jit::PortDirection::output);
    auto window = find_port("window", iv::PortKind::sample, iv::graph_jit::PortDirection::output);
    ASSERT_NE(source, plan.ports.end());
    ASSERT_NE(curve, plan.ports.end());
    ASSERT_NE(window, plan.ports.end());
    EXPECT_EQ(source->max_event_count, 3u);
    EXPECT_TRUE(plan.nodes[curve->node].authored_tock_execution);
    EXPECT_FALSE(plan.nodes[curve->node].replays_tick);
    EXPECT_TRUE(plan.nodes[curve->node].authored_forward_coverage);
    EXPECT_TRUE(plan.nodes[curve->node].authored_reverse_coverage);
    auto const source_port = static_cast<iv::graph_jit::BackgroundPortIndex>(source - plan.ports.begin());
    auto const curve_port = static_cast<iv::graph_jit::BackgroundPortIndex>(curve - plan.ports.begin());
    auto const window_port = static_cast<iv::graph_jit::BackgroundPortIndex>(window - plan.ports.begin());
    auto const stored_window = std::ranges::find_if(plan.storage.ports, [&](auto const& storage) {
        return storage.output_port == window_port
            && storage.storage == iv::graph_jit::PortStorageKind::persisted_data;
    });
    ASSERT_NE(stored_window, plan.storage.ports.end());
    auto const window_storage = static_cast<iv::graph_jit::PortStorageIndex>(stored_window - plan.storage.ports.begin());
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    iv::TickMaterializationStore materializations;
    auto tock_states = graph.tock_state_layout.create_storage(resources);
    tock_states.initialize();
    iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
    iv::BackgroundPropagationWorkspace propagation(plan, graph.specialization.sample_rate);
    iv::CoveragePropagationRequest roots{
        .locally_changed_nodes = {source->node, curve->node, window->node},
        .output_demands = {{.port = window_port, .required = iv::IndexCoverage{range(2, 4)}}}};
    for (auto const& port : plan.ports) {
        if (port.name == "bound" && port.direction == iv::graph_jit::PortDirection::output)
            roots.locally_changed_nodes.push_back(port.node);
    }
    iv::BackgroundEvaluationTransaction transaction{
        graph, tock_states, coverage, propagation, data, materializations,
        {.semantic_version = 1, .roots = std::move(roots)}};
    auto result = transaction.execute();
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);
    auto const replay_demand = std::ranges::find(result->coverage.output_requirements,
        curve_port, &iv::RequiredOutputCoverage::port);
    ASSERT_NE(replay_demand, result->coverage.output_requirements.end());
    EXPECT_EQ(replay_demand->required, iv::IndexCoverage{range(2, 4)});
    auto const point_demand = std::ranges::find(result->coverage.output_requirements,
        source_port, &iv::RequiredOutputCoverage::port);
    ASSERT_NE(point_demand, result->coverage.output_requirements.end());
    EXPECT_TRUE(point_demand->required.contains(index(0)));
    EXPECT_TRUE(point_demand->required.contains(index(36000LL * 88200)));
    std::size_t bounded_sources = 0;
    for (auto const& requirement : result->coverage.output_requirements) {
        if (plan.ports[requirement.port].name != "bound") continue;
        ++bounded_sources;
        EXPECT_FALSE(requirement.required.contains(index(0)));
        EXPECT_FALSE(requirement.required.contains(index(4)));
        EXPECT_TRUE(requirement.required.contains(index(2)));
        EXPECT_TRUE(requirement.required.contains(index(3)));
        EXPECT_FALSE(requirement.required.contains(index(36000LL * 88200)));
    }
    EXPECT_EQ(bounded_sources, 2u);
    auto const handle = data.resolve_output(iv::persisted_output_id(plan, window_storage,
        graph.project_generation));
    auto pinned = reader.pin();
    auto const* region = pinned->find_sample_region(handle, index(2));
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(region->range, range(2, 4));
    // Interpolation produces .5 and .75. The upper input clips only the second sample.
    EXPECT_FLOAT_EQ(static_cast<float>(region->at(index(2), 0)), .5f);
    EXPECT_FLOAT_EQ(static_cast<float>(region->at(index(3), 0)), .6f);
}

TEST_F(GraphJitRuntimeFixture, TransformedCurvePointsDetermineCoverageBeforeSequentialManualFallback)
{
    for (bool empty : {false, true}) {
        SCOPED_TRACE(empty);
        auto compiled = compile_with_manual_input(empty ? "iv.test.automation.empty_sparse_curve"
            : "iv.test.automation.transformed_sparse_curve", 711 + empty, "gain", .25f);
        ASSERT_TRUE(compiled.succeeded())
            << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
        auto const& graph = *compiled.compiled_graph;
        auto const& plan = graph.background_evaluation_plan;
        auto find_port = [&](std::string_view name, iv::PortKind kind,
                             iv::graph_jit::PortDirection direction) {
            return std::ranges::find_if(plan.ports, [&](auto const& port) {
                return port.name == name && port.kind == kind && port.direction == direction;
            });
        };
        auto curve = find_port("value", iv::PortKind::sample, iv::graph_jit::PortDirection::output);
        auto window = find_port("window", iv::PortKind::sample, iv::graph_jit::PortDirection::output);
        ASSERT_NE(curve, plan.ports.end());
        ASSERT_NE(window, plan.ports.end());
        auto const curve_port = static_cast<iv::graph_jit::BackgroundPortIndex>(curve - plan.ports.begin());
        auto const window_port = static_cast<iv::graph_jit::BackgroundPortIndex>(window - plan.ports.begin());
        auto storage = std::ranges::find(plan.storage.ports, std::optional{window_port}, &iv::graph_jit::PortStoragePlan::output_port);
        ASSERT_NE(storage, plan.storage.ports.end());
        iv::PersistedDataStore data;
        auto reader = data.register_reader();
        auto states = graph.tock_state_layout.create_storage(resources);
        states.initialize();
        iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
        iv::BackgroundPropagationWorkspace propagation(plan, graph.specialization.sample_rate);
        iv::CoveragePropagationRequest roots;
        for (std::size_t node = 0; node < plan.nodes.size(); ++node) roots.locally_changed_nodes.push_back(node);
        roots.output_demands.push_back({.port = window_port, .required = iv::IndexCoverage{range(0, 8)}});
        iv::BackgroundEvaluationTransaction transaction{graph, states, coverage, propagation, data,
            {.semantic_version = 1, .roots = std::move(roots)}};
        auto result = transaction.execute();
        ASSERT_TRUE(result.has_value()) << result.error();
        auto changed = std::ranges::find(result->coverage.output_changes, curve_port, &iv::PropagatedOutputChange::port);
        // The transformer empties the point at index 4. Construction arguments of
        // the original source would incorrectly predict a hold through index 12.
        if (empty) {
            EXPECT_TRUE(changed == result->coverage.output_changes.end() || changed->coverage.empty());
        } else {
            ASSERT_NE(changed, result->coverage.output_changes.end());
            EXPECT_EQ(changed->coverage, iv::IndexCoverage{range(0, 4)});
        }
        auto const handle = data.resolve_output(iv::persisted_output_id(plan,
            static_cast<iv::graph_jit::PortStorageIndex>(storage - plan.storage.ports.begin()), graph.project_generation));
        auto pinned = reader.pin();
        auto const* values = pinned->find_sample_region(handle, index(0));
        ASSERT_NE(values, nullptr);
        // The replayable Tick adds 1. Its independently stored manual value .25
        // supplies the gap, rather than becoming another fan-in contribution.
        std::array<float, 8> const expected{1, 1.5f, 2, 2.5f, 1.25f, 1.25f, 1.25f, 1.25f};
        for (std::size_t i = 0; i < expected.size(); ++i)
            EXPECT_FLOAT_EQ(values->at(index(static_cast<std::int64_t>(i)), 0).value, empty ? 1.25f : expected[i]) << i;
    }
}

TEST_F(GraphJitRuntimeFixture, CoverageManualInputAddsDataOnlyInGaps)
{
    struct Demand { char const* input; std::int64_t begin, end; };
    for (auto const [input, begin, end] : {Demand{"", 18, 26}, Demand{"in", 18, 26},
             Demand{"coverage", 18, 26}, Demand{"in", 20, 24}, Demand{"in", 24, 26}}) {
        SCOPED_TRACE(::testing::Message{} << input << " [" << begin << ", " << end << ")");
        bool const enabled = *input != '\0';
        auto const fixture = "iv.test.replay.coverage_manual_window";
        auto compiled = enabled ? compile_with_manual_input(fixture, 718, input, .25f)
                                : compile(fixture, 718);
        ASSERT_TRUE(compiled.succeeded())
            << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
        auto const& graph = *compiled.compiled_graph;
        auto const& plan = graph.background_evaluation_plan;
        auto const window = std::ranges::find(plan.ports, std::string{"window"}, &iv::graph_jit::BackgroundPortPlan::name);
        ASSERT_NE(window, plan.ports.end());
        auto const window_port = static_cast<iv::graph_jit::BackgroundPortIndex>(window - plan.ports.begin());
        auto const storage = std::ranges::find(plan.storage.ports, std::optional{window_port}, &iv::graph_jit::PortStoragePlan::output_port);
        ASSERT_NE(storage, plan.storage.ports.end());
        iv::PersistedDataStore data;
        auto reader = data.register_reader();
        auto states = graph.tock_state_layout.create_storage(resources);
        states.initialize();
        iv::BackgroundCoverageState coverage(plan.accumulators.output_change_count);
        iv::BackgroundPropagationWorkspace propagation(plan, graph.specialization.sample_rate);
        iv::CoveragePropagationRequest roots;
        for (std::size_t node = 0; node < plan.nodes.size(); ++node) roots.locally_changed_nodes.push_back(node);
        roots.output_demands.push_back({.port = window_port, .required = iv::IndexCoverage{range(begin, end)}});
        iv::BackgroundEvaluationTransaction transaction{graph, states, coverage, propagation, data,
            {.semantic_version = 1, .roots = std::move(roots)}};
        auto result = transaction.execute();
        ASSERT_TRUE(result.has_value()) << result.error();
        ASSERT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);
        auto const target = std::ranges::find_if(plan.ports, [&](auto const& port) {
            return port.node == window->node && port.direction == iv::graph_jit::PortDirection::input;
        });
        ASSERT_NE(target, plan.ports.end());
        auto const manual = std::ranges::find(plan.ports, std::string{"manual"}, &iv::graph_jit::BackgroundPortPlan::name);
        if (enabled) {
            ASSERT_NE(manual, plan.ports.end());
            auto const manual_port = static_cast<iv::graph_jit::BackgroundPortIndex>(manual - plan.ports.begin());
            auto const changed = std::ranges::find(result->coverage.output_changes, manual_port, &iv::PropagatedOutputChange::port);
            ASSERT_NE(changed, result->coverage.output_changes.end());
            EXPECT_TRUE(changed->coverage.contains(index(18)));
            EXPECT_FALSE(changed->coverage.contains(index(20)));
            EXPECT_TRUE(changed->coverage.contains(index(25)));
        } else EXPECT_EQ(manual, plan.ports.end());
        auto const handle = data.resolve_output(iv::persisted_output_id(plan,
            static_cast<iv::graph_jit::PortStorageIndex>(storage - plan.storage.ports.begin()), graph.project_generation));
        auto pinned = reader.pin();
        float const gap = enabled ? .25f : 0.f;
        std::array<float, 8> const expected{gap, gap, 4, 5, 6, 7, gap, gap};
        for (auto i = begin; i < end; ++i) {
            auto const* values = pinned->find_sample_region(handle, index(i));
            ASSERT_NE(values, nullptr) << i;
            EXPECT_FLOAT_EQ(values->at(index(i), 0).value, expected[static_cast<std::size_t>(i - 18)]) << i;
        }
    }
}

TEST_F(GraphJitRuntimeFixture, AutomationFeedsAModuleInputAcrossRollingPlaybackWindows)
{
    auto compiled = compile("iv.test.automation.rolling", 713);
    ASSERT_TRUE(compiled.succeeded())
        << (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message);
    auto const& graph = *compiled.compiled_graph;
    ASSERT_FALSE(graph.background_evaluation_plan.tick_sequential_inputs.empty());
    iv::GraphExecutor executor{resources};
    ASSERT_EQ(executor.stage(compiled.compiled_graph), iv::GraphExecutorStageResult::accepted);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool activated = false;
    while (!(activated = executor.activate_pending(index(0)))
        && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    ASSERT_TRUE(activated);
    auto const block = graph.specialization.block_size;
    // Use the executor's automatic reserve. Moving past it forces partial
    // retention and refills, unlike a fixed explicit preparation window.
    for (std::size_t step = 0; step < 32; ++step) {
        for (;;) {
            auto const before = executor.playback_underrun_blocks();
            executor.tick_block(index(static_cast<std::int64_t>(step * block)), block);
            ASSERT_FALSE(executor.background_execution_failures().any()) << step;
            if (executor.playback_underrun_blocks() == before) break;
            ASSERT_LT(std::chrono::steady_clock::now(), deadline) << step;
            std::this_thread::yield();
        }
    }
    EXPECT_FALSE(executor.background_execution_failures().any());
}

TEST(AutomationPointSource, MergesExactSampleIndicesWithLegacySecondsAcrossTheSignedSeam)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(iv::details::iv_builder_session_create(), iv::details::iv_builder_session_destroy);
    iv::GraphBuilder graph(session.get());
    using iv::automation::Point;
    using iv::automation::SampleIndex;
    auto const rate = 48000u;
    auto const inputs = std::array{
        Point{.id = 3, .seconds = 0.0},
        Point{.id = 1, .sample_index = SampleIndex{"-9223372036854775808"}},
        Point{.id = 2, .sample_index = SampleIndex{"9223372036854775807"}},
    };
    iv::automation::PointSource source(graph.immutable_array<Point>(inputs));
    EXPECT_EQ(iv::automation::point_index(inputs[1], rate).signed_value(), std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(iv::automation::point_index(inputs[2], rate).signed_value(), std::numeric_limits<std::int64_t>::max());
    auto const coverage = source.range(rate);
    for (auto const& point : inputs) EXPECT_TRUE(coverage.contains(iv::automation::point_index(point, rate)));
    EXPECT_FALSE(coverage.contains(index(-1)));

    iv::automation::PointSource adjacent(graph.immutable_array<Point>(std::span(inputs).subspan(1)));
    EXPECT_EQ(adjacent.range(rate), (iv::GlobalIndexRange{iv::automation::point_index(inputs[2], rate), 2}));
}

TEST_F(GraphJitRuntimeFixture, AutomationMessagesSerializeCompleteReplacementPointLists)
{
    using Session = std::unique_ptr<iv::details::BuilderSession,
        decltype(&iv::details::iv_builder_session_destroy)>;
    Session session(iv::details::iv_builder_session_create(), iv::details::iv_builder_session_destroy);
    auto root = revision->package_root.generic_string();
    std::array packages{iv::details::BuilderPackageView{
        .package_root = root, .definitions = revision->provider_definitions,
        .config_pointer_fields = revision->config_pointer_fields,
        .retained_globals = revision->retained_globals,
        .node_message_handlers = revision->node_message_handlers,
        .module_message_handlers = revision->module_message_handlers,
        .node_state_structures = revision->node_state_structures,
    }};
    iv::details::set_builder_packages(session.get(), packages);
    std::vector<iv::ConfiguredNodeMessageHandler> handlers;
    {
        iv::GraphBuilder graph(session.get());
        (void)iv::details::configure_package_definition_provider(
            graph, "iv.test.automation.window_module", std::nullopt, {});
        graph.outputs();
        handlers = iv::details::take_builder_message_handlers(session.get());
        (void)iv::details::take_built_graph(session.get());
    }
    session.reset();
    ASSERT_EQ(handlers.size(), 5u);
    struct Capture {
        std::string definition;
        std::string arguments;
    } capture;
    iv::NodeMessageContextOperations operations{
        .context = &capture,
        .replace = +[](void* data, iv::NodeAddress const&, std::string definition,
                      std::string arguments) {
            auto& value = *static_cast<Capture*>(data);
            value.definition = std::move(definition);
            value.arguments = std::move(arguments);
        },
    };
    auto invoke = [&](std::string_view type, nlohmann::ordered_json payload) {
        auto handler = std::ranges::find_if(handlers, [&](auto const& item) {
            return iv::details::configuration_type_display_name(item.event_type).ends_with(type);
        });
        EXPECT_NE(handler, handlers.end());
        if (handler == handlers.end()) return nlohmann::ordered_json{};
        auto event = iv::evaluate_configuration_json_value(payload, *handler->event_type);
        capture = {};
        handler->invoke(handler->receiver.get(), operations,
            iv::NodeAddress{.instance_id = "curve"}, handler->module_local_refs.get(), event.data());
        EXPECT_EQ(capture.definition, "iv.builtin.automation.curve");
        return nlohmann::ordered_json::parse(capture.arguments);
    };
    auto added = invoke("AddPoint", {{"point", {{"id", 4}, {"seconds", 1.0}, {"value", -2.0}, {"interpolation", "hold"}, {"tension", 1.0}}}});
    ASSERT_TRUE(added.is_array());
    ASSERT_EQ(added.size(), 1u);
    ASSERT_EQ(added[0].size(), 4u);
    // The stored list is sorted; the handler appends without sorting.
    EXPECT_EQ(added[0][0]["id"], 1);
    EXPECT_EQ(added[0][2]["id"], 3);
    EXPECT_EQ(added[0][3]["id"], 4);
    EXPECT_EQ(added[0][3]["value"], -2.0);
    EXPECT_EQ(added[0][3]["interpolation"], "hold");
    auto const exact = invoke("AddPoint", {{"point", {{"id", 4}, {"value", .25}, {"sample_index", "9007199254740993"}}}});
    EXPECT_EQ(exact[0][3]["sample_index"], "9007199254740993");
    auto placed = invoke("PlacePoint", {
        {"insert", true},
        {"point", {{"id", 4}, {"sample_index", "9007199254740993"}, {"value", .5},
            {"interpolation", "empty"}, {"tension", 1.0}}},
        {"segments", {{{"id", 3}, {"interpolation", "hold"}, {"tension", .5}}}},
    });
    ASSERT_EQ(placed[0].size(), 4u);
    EXPECT_EQ(placed[0][2]["interpolation"], "hold");
    EXPECT_EQ(placed[0][2]["tension"], .5);
    EXPECT_EQ(placed[0][3]["interpolation"], "empty");
    EXPECT_EQ(placed[0][3]["sample_index"], "9007199254740993");
    // Persist the chosen order even when a legacy point and an exact point
    // occupy the same sample. The point IDs still identify their values.
    auto const coincident = invoke("PlacePoint", {
        {"insert", true}, {"point", {{"id", 4}, {"sample_index", "0"}, {"value", .5}}},
        {"ordered_ids", {4, 1}},
    });
    EXPECT_EQ(coincident[0][0]["id"], 1);
    EXPECT_EQ(coincident[0][0]["order"], 1);
    EXPECT_EQ(coincident[0][3]["id"], 4);
    EXPECT_EQ(coincident[0][3]["order"], 0);
    EXPECT_TRUE(iv::automation::point_order_less({.id = 4}, {.id = 1, .order = 1}));
    EXPECT_THROW(invoke("PlacePoint", {
        {"insert", true}, {"point", {{"id", 4}}}, {"ordered_ids", {4, 4}},
    }), std::invalid_argument);
    EXPECT_TRUE(capture.definition.empty());
    EXPECT_THROW(invoke("PlacePoint", {
        {"insert", true}, {"point", {{"id", 4}}}, {"segments", {{{"id", 99}}}},
    }), std::invalid_argument);
    EXPECT_TRUE(capture.definition.empty());
    auto edited = invoke("SetSegment", {{"id", 2}, {"interpolation", "empty"}, {"tension", .5}});
    EXPECT_EQ(edited[0][1]["interpolation"], "empty");
    EXPECT_EQ(edited[0][1]["tension"], .5);
    EXPECT_EQ(edited[0][1]["value"], 1);
    EXPECT_THROW(invoke("SetSegment", {{"id", 2}, {"interpolation", "bezier"}, {"tension", 2.0}}), std::invalid_argument);
    auto removed = invoke("RemovePoint", {{"id", 2}});
    ASSERT_TRUE(removed.is_array());
    ASSERT_EQ(removed.size(), 1u);
    ASSERT_EQ(removed[0].size(), 2u);
    EXPECT_EQ(removed[0][0]["id"], 1);
    EXPECT_EQ(removed[0][1]["id"], 3);
    auto moved = invoke("MovePoint", {{"id", 2}, {"seconds", 1.0}, {"value", 2.0}});
    ASSERT_TRUE(moved.is_array());
    ASSERT_EQ(moved[0].size(), 3u);
    EXPECT_EQ(moved[0][1]["id"], 2);
    EXPECT_EQ(moved[0][1]["seconds"], 1.0);
    EXPECT_EQ(moved[0][0]["value"], 0);
    EXPECT_EQ(moved[0][1]["value"], 2.0);
    EXPECT_EQ(moved[0][1]["interpolation"], "bezier");
    EXPECT_EQ(moved[0][1]["tension"], 0);
    auto const exact_move = invoke("MovePoint", {{"id", 2}, {"sample_index", "-9007199254740993"}, {"value", .75}});
    EXPECT_EQ(exact_move[0][1]["sample_index"], "-9007199254740993");
    EXPECT_THROW((void)iv::automation::SampleIndex("9223372036854775808"), std::invalid_argument);
    EXPECT_THROW(invoke("RemovePoint", {{"id", 99}}), std::invalid_argument);
}

TEST(AutomationCurveBounds, CrossedSignalsCollapseAndUncoveredSidesStayOpen)
{
    using iv::automation::clamp_output_value;
    EXPECT_EQ(clamp_output_value(-2, 6, 2), 4);
    EXPECT_EQ(clamp_output_value(8, 6, 2), 4);
    EXPECT_EQ(clamp_output_value(-2, 6, 6), 6);
    EXPECT_EQ(clamp_output_value(-2, 0, std::nullopt), 0);
    EXPECT_EQ(clamp_output_value(-2, std::nullopt, 1), -2);
    EXPECT_EQ(clamp_output_value(10, std::nullopt, std::nullopt), 10);
}

} // namespace iv::test::graph_jit_tests
