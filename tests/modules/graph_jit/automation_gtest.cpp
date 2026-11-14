#include "test_support.h"

namespace iv::test::graph_jit_tests {

TEST_F(GraphJitRuntimeFixture, SparseTockPointsReplayOnlyTheRequestedCurveWindow)
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
    EXPECT_TRUE(plan.nodes[curve->node].replays_tick);
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
    iv::BackgroundEvaluationTransaction transaction{
        graph, tock_states, coverage, propagation, data, materializations,
        {.semantic_version = 1,
         .roots = {.locally_changed_nodes = {source->node, curve->node, window->node},
                   .output_demands = {{.port = window_port, .required = iv::IndexCoverage{range(2, 4)}}}}}};
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
    auto const handle = data.resolve_output(iv::persisted_output_id(plan, window_storage,
        graph.project_generation));
    auto pinned = reader.pin();
    auto const* region = pinned->find_sample_region(handle, index(2));
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(region->range, range(2, 4));
    EXPECT_FLOAT_EQ(static_cast<float>(region->at(index(2), 0)), .5f);
    EXPECT_FLOAT_EQ(static_cast<float>(region->at(index(3), 0)), .75f);
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
    ASSERT_EQ(handlers.size(), 3u);
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
        EXPECT_EQ(capture.definition, "iv.automation.curve");
        return nlohmann::ordered_json::parse(capture.arguments);
    };
    auto added = invoke("AddPoint", {{"point", {{"id", 4}, {"seconds", 1.0}, {"value", .25}}}});
    ASSERT_TRUE(added.is_array());
    ASSERT_EQ(added.size(), 2u);
    ASSERT_EQ(added[0].size(), 4u);
    EXPECT_EQ(added[0][3]["id"], 4);
    EXPECT_EQ(added[1], 36000);
    auto removed = invoke("RemovePoint", {{"id", 2}});
    ASSERT_TRUE(removed.is_array());
    ASSERT_EQ(removed[0].size(), 2u);
    EXPECT_EQ(removed[0][0]["id"], 1);
    EXPECT_EQ(removed[0][1]["id"], 3);
    auto moved = invoke("MovePoint", {{"id", 2}, {"seconds", 1.0}, {"value", .75}});
    ASSERT_TRUE(moved.is_array());
    ASSERT_EQ(moved[0].size(), 3u);
    EXPECT_EQ(moved[0][1]["id"], 2);
    EXPECT_EQ(moved[0][1]["seconds"], 1.0);
    EXPECT_EQ(moved[0][1]["value"], .75);
    EXPECT_THROW(invoke("RemovePoint", {{"id", 99}}), std::invalid_argument);
}

} // namespace iv::test::graph_jit_tests
