#include "../module_test_utils.h"

#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/graph_input_lanes_timeline_bridge.h>
#include <intravenous/runtime/iv_module_definitions_iv_module_instances_bridge.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/iv_module_instances_graph_input_lanes_bridge.h>
#include <intravenous/runtime/iv_module_instances_iv_module_source_introspection_bridge.h>
#include <intravenous/runtime/lane_filters.h>
#include <intravenous/runtime/timeline_lane_filters_bridge.h>
#include <intravenous/runtime/iv_module_source_introspection.h>
#include <intravenous/runtime/iv_module_source_introspection_graph_input_lanes_bridge.h>
#include <intravenous/runtime/timeline.h>
#include <intravenous/node/block_executor.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <ranges>
#include <set>
#include <string>
#include <thread>

namespace {
using namespace std::chrono_literals;
using iv::test_support::mutable_module_fixture_workspace;
using iv::test_support::make_inline_module_workspace;
using iv::test_support::read_only_module_fixture_workspace;
using iv::test_support::shared_inline_module_workspace;

std::string source_text(iv::LiveSourceSpan const& span)
{
    auto const map = iv::SourceTextLineMap::from_file(span.file_path);
    auto const begin = map.offset_for(span.range.start);
    auto const end = map.offset_for(span.range.end);
    return map.text.substr(begin, end - begin);
}

struct SeededIvModuleSourceIntrospectionApp {
    iv::Timeline timeline;
    iv::IvModuleInstances instances;
    iv::IvModuleDefinitions definitions;
    iv::GraphInputLanes graph_input_lanes;
    iv::LaneFilters lane_filters;
    iv::IvModuleSourceIntrospection introspection;
    iv::StartupConfig startup_config;
    iv::timeline_lane_filters_bridge::scope timeline_lane_filters_scope;
    iv::iv_module_definitions_iv_module_instances_bridge::scope
        iv_module_definitions_iv_module_instances_scope;
    iv::iv_module_instances_iv_module_source_introspection_bridge::scope
        iv_module_instances_iv_module_source_introspection_scope;
    iv::iv_module_instances_graph_input_lanes_bridge::scope
        iv_module_instances_graph_input_lanes_scope;
    iv::iv_module_source_introspection_graph_input_lanes_bridge::scope
        iv_module_source_introspection_graph_input_lanes_scope;
    iv::graph_input_lanes_timeline_bridge::scope
        graph_input_lanes_timeline_scope;

    SeededIvModuleSourceIntrospectionApp(
        std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots)
        : startup_config(
              std::move(workspace_root),
              std::move(discovery_start),
              std::move(extra_search_roots)),
          timeline_lane_filters_scope(timeline, lane_filters),
          iv_module_definitions_iv_module_instances_scope(definitions, instances),
          iv_module_instances_iv_module_source_introspection_scope(
              instances,
              introspection),
          iv_module_instances_graph_input_lanes_scope(
              instances,
              graph_input_lanes),
          iv_module_source_introspection_graph_input_lanes_scope(
              introspection,
              graph_input_lanes),
          graph_input_lanes_timeline_scope(graph_input_lanes, timeline)
    {}

    auto initialize()
    {
        auto const config = startup_config.initialize();
        auto const module_root = std::filesystem::weakly_canonical(config.workspace_root);
        auto definition = iv::test::load_runtime_iv_module_definition(config, module_root);
        (void)instances.create_instance(
            definition.module_id,
            module_root,
            "instance:1");
        definitions.seed_loaded_definition(std::move(definition));
    }

    auto query_by_spans(
        std::filesystem::path const &file_path,
        std::vector<iv::SourceRange> const &ranges,
        iv::SourceRangeMatchMode match_mode = iv::SourceRangeMatchMode::intersection) const
    {
        return introspection.query_by_spans(file_path, ranges, match_mode);
    }

    auto query_active_regions(std::filesystem::path const &file_path) const
    {
        return introspection.query_active_regions(file_path);
    }

    auto get_virtual_node(std::string const &node_id) const
    {
        return introspection.get_virtual_node(node_id);
    }

    auto get_virtual_nodes(std::vector<std::string> const &node_ids) const
    {
        return introspection.get_virtual_nodes(node_ids);
    }

    void set_sample_input_value(
        std::string const &node_id,
        size_t input_ordinal,
        iv::Sample value,
        std::optional<size_t> member_ordinal = std::nullopt)
    {
        graph_input_lanes.set_sample_input_value(
            iv::ProjectSetSampleInputValueRequest {
                .node_id = node_id,
                .member_ordinal = member_ordinal,
                .input_ordinal = input_ordinal,
                .value = value,
            });
    }

    void set_sample_input_state(
        std::string const &node_id,
        size_t input_ordinal,
        iv::ProjectSampleInputState state,
        std::optional<size_t> member_ordinal = std::nullopt)
    {
        graph_input_lanes.set_sample_input_state(
            iv::ProjectSetSampleInputStateRequest {
                .node_id = node_id,
                .member_ordinal = member_ordinal,
                .input_ordinal = input_ordinal,
                .state = state,
            });
    }
};
} // namespace

TEST(IvModuleSourceIntrospection, QueryBySpansReturnsMatchingLiveNodesWithPorts)
{
    auto const workspace = read_only_module_fixture_workspace("local_cmake");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {
            iv::SourceRange{
                .start = {.line = 7, .column = 1},
                .end = {.line = 18, .column = 1},
            },
        });
    ASSERT_FALSE(result.nodes.empty());

    auto const &node = result.nodes.front();
    EXPECT_FALSE(node.id.empty());
    EXPECT_FALSE(node.kind.empty());
    EXPECT_FALSE(node.source_spans.empty());
    bool has_any_port =
        !node.sample_inputs.empty() || !node.sample_outputs.empty() || !node.event_inputs.empty() ||
        !node.event_outputs.empty();
    EXPECT_TRUE(has_any_port);
}

TEST(IvModuleSourceIntrospection, AliasedStateIsFinalizedWithStructuralMetadata)
{
    auto const workspace = make_inline_module_workspace(
        "iv_module_source_introspection_aliased_state",
        R"(#include <intravenous/dsl.h>

#include <cstdint>

namespace {
    struct StatePayload {
        unsigned int phase = 7;
        float gain = 0.5f;
    };

    struct StateCarrier {
        using State = StatePayload;
    };

    struct AliasedStateNode {
        using State = StatePayload;

        static constexpr auto outputs()
        {
            return std::array<iv::OutputConfig, 1>{};
        }

        void tick(iv::TickSampleContext<AliasedStateNode> const& ctx) const
        {
            auto& state = ctx.state();
            ctx.outputs[0].push(state.gain);
            ++state.phase;
        }
    };

    struct InheritedStateNode : StateCarrier {
        static constexpr auto outputs()
        {
            return std::array<iv::OutputConfig, 1>{};
        }

        void tick(iv::TickSampleContext<InheritedStateNode> const& ctx) const
        {
            auto& state = ctx.state();
            ctx.outputs[0].push(state.gain);
            ++state.phase;
        }
    };

    struct ScalarStateNode {
        using State = std::int32_t;

        static constexpr auto outputs()
        {
            return std::array<iv::OutputConfig, 1>{};
        }

        void initialize(iv::InitializationContext<ScalarStateNode> const& ctx) const
        {
            ctx.state() = 11;
        }

        void tick(iv::TickSampleContext<ScalarStateNode> const& ctx) const
        {
            ctx.outputs[0].push(static_cast<float>(ctx.state()));
        }
    };

    void aliased_state_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const direct = details::configure_concrete_node<AliasedStateNode>(g);
        auto const inherited = details::configure_concrete_node<InheritedStateNode>(g);
        auto const scalar = details::configure_concrete_node<ScalarStateNode>(g);
        g.outputs(
            "direct"_P = direct,
            "inherited"_P = inherited,
            "scalar"_P = scalar);
    }
}
)");

    auto loader = iv::test::make_loader();
    auto definition = loader.load_package_definitions(workspace).front();
    auto executor = iv::BlockNodeExecutor::create(
        iv::TypeErasedNode(definition.root), 8);

    auto structural_state_nodes = 0u;
    for (auto const& record : executor.layout().nodes) {
        auto const has_phase = record.node_state_structure
            && std::ranges::any_of(
                record.node_state_structure->fields,
                [](iv::NodeStateFieldStructure const& field) {
                    return field.name == "phase";
                });
        if (has_phase) {
            ++structural_state_nodes;
            ASSERT_EQ(record.node_state_structure->fields.size(), 2u);
            EXPECT_FALSE(record.node_state_structure->fields.front().type_name.empty());
        }
    }
    // NodeState<Node>::Type accepts both a direct alias and an alias found by
    // normal base-class lookup. Both must reach the finalized runtime layout.
    EXPECT_EQ(structural_state_nodes, 2u);

    auto scalar_state_nodes = 0u;
    for (auto const& record : executor.layout().nodes) {
        if (!record.node_state_structure
            || record.node_state_structure->size_bits != sizeof(std::int32_t) * 8
            || !record.node_state_structure->fields.empty()) {
            continue;
        }
        ++scalar_state_nodes;
    }
    EXPECT_EQ(scalar_state_nodes, 1u);
}

TEST(IvModuleSourceIntrospection, QueryBySpansKeepsDistinctDeclarationsSeparate)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_merged_virtual",
        R"(#include <intravenous/dsl.h>

namespace {
    template<int I>
    iv::NodeRef make_value(iv::GraphBuilder& g)
    {
        return iv::details::configure_concrete_node<iv::Constant>(
            g, static_cast<iv::Sample>(I)).node_ref();
    }

    void merged_virtual_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const a = make_value<0>(g);
        auto const b = make_value<1>(g);
        auto const sink = a + b;
        g.outputs("main"_P = sink);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 22, .column = 1}}});

    size_t constant_count = 0;
    for (auto const &node : result.nodes) {
        if (!node.kind.contains("Constant")) continue;
        ++constant_count;
        EXPECT_EQ(node.member_count, 1u);
        EXPECT_FALSE(node.source_spans.empty());
    }
    EXPECT_EQ(constant_count, 2u);
}

TEST(IvModuleSourceIntrospection, GenericChannelOutputArgumentsArePublicOutputSourceSpans)
{
    auto const workspace = make_inline_module_workspace(
        "iv_module_source_introspection_generic_channel_outputs",
        R"(#include <intravenous/dsl.h>

namespace {
    void generic_channel_outputs(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const source = details::configure_concrete_node<Constant>(g, 0.25f);
        g.outputs("main"_P[stereo::left] = source, "main"_P[stereo::right] = source);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();
    app.introspection.set_public_sample_outputs(app.graph_input_lanes.public_sample_outputs());
    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 16, .column = 1}}});
    std::vector<iv::VirtualNodeInfo const*> outputs;
    for (auto const& node : result.nodes) if (node.kind == "Public output") outputs.push_back(&node);
    ASSERT_EQ(outputs.size(), 2u);
    for (auto const* output : outputs) {
        ASSERT_FALSE(output->source_spans.empty());
        ASSERT_EQ(output->sample_outputs.size(), 1u);
        EXPECT_EQ(output->sample_outputs.front().connectivity, iv::VirtualPortConnectivity::connected);
        EXPECT_EQ(output->sample_outputs.front().state_value, "timelineLane");
        ASSERT_EQ(output->members.size(), 2u);
        for (auto const& member : output->members) {
            ASSERT_EQ(member.sample_outputs.size(), 1u);
            EXPECT_EQ(member.sample_outputs.front().state_value, "virtualFollow");
            EXPECT_EQ(member.sample_outputs.front().connectivity, iv::VirtualPortConnectivity::connected);
        }
    }
}

TEST(IvModuleSourceIntrospection, TypedPublicInputsAndCapturedBuilderOutputsRemainSourceAnnotated)
{
    auto const workspace = make_inline_module_workspace(
        "iv_module_source_introspection_typed_public_ports_in_lambda",
        R"(#include <intravenous/dsl.h>

namespace {
    void typed_public_ports_in_lambda(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const frequency = g.input<"frequency">(220.0);
        auto const detune = g.input<"detune">(2.5);
        auto emit = [&] {
            g.outputs("main"_P = frequency + detune);
        };
        emit();
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const inputs = app.graph_input_lanes.public_sample_inputs();
    ASSERT_EQ(inputs.size(), 2u);
    for (auto const& input : inputs) {
        EXPECT_FALSE(input.source_identity.empty());
        EXPECT_FALSE(input.source_infos.empty());
    }
    auto const outputs = app.graph_input_lanes.public_sample_outputs();
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_FALSE(outputs.front().source_identity.empty());
    EXPECT_FALSE(outputs.front().source_infos.empty());

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 20, .column = 1}}});
    size_t public_inputs = 0;
    size_t public_outputs = 0;
    for (auto const& node : result.nodes) {
        if (node.kind == "Public input") ++public_inputs;
        if (node.kind == "Public output") ++public_outputs;
    }
    EXPECT_EQ(public_inputs, 2u);
    EXPECT_EQ(public_outputs, 1u);
}

TEST(IvModuleSourceIntrospection, EmbeddedModulePublicInputIdentifiersRemainPortAnnotated)
{
    auto const workspace = make_inline_module_workspace(
        "iv_module_source_introspection_embedded_public_input_identifiers",
        R"(#include <intravenous/dsl.h>

namespace {
    void child_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const in = g.input<"in">();
        auto const az = g.input<"azimuth">(0, -180, 180);
        g.outputs("main"_P = in + az);
    }
    IV_MODULE("iv.test.source_provenance.child", child_module);

    void module_main(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const child = g.node<"iv.test.source_provenance.child">();
        g.outputs("main"_P = child);
    }
    IV_MODULE("iv.test.source_provenance.main", module_main);
}
)");

    auto loader = iv::test::make_loader();
    auto definitions = loader.load_package_definitions(workspace);
    auto const definition = std::ranges::find_if(definitions, [](auto const& candidate) {
        return candidate.module_id == "iv.test.source_provenance.main";
    });
    ASSERT_NE(definition, definitions.end());

    auto const span_text = [](iv::SourceSpan const& span) {
        auto const text = iv::test::read_text(span.file_path);
        return text.substr(span.begin, span.end - span.begin);
    };
    auto has_port_identifier = [&](std::string_view port_name,
                                   std::string_view identifier) {
        return std::ranges::any_of(
            definition->introspection.virtual_nodes,
            [&](auto const& node) {
                return std::ranges::any_of(
                    node.sample_inputs,
                    [&](auto const& port) {
                        return port.name == port_name
                            && std::ranges::any_of(
                                port.source_spans,
                                [&](auto const& span) {
                                    return span_text(span) == identifier;
                                });
                    });
            });
    };

    EXPECT_TRUE(has_port_identifier("in", "in"));
    EXPECT_TRUE(has_port_identifier("azimuth", "az"));
}

TEST(IvModuleSourceIntrospection, SourceProvenanceUsesOnlyIdentifiersAndNamedBindingStrings)
{
    auto const workspace = make_inline_module_workspace(
        "iv_module_source_introspection_token_provenance",
        R"(#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/shaping.h>

namespace {
    struct TriggerSource {
        static constexpr auto event_outputs()
        {
            return std::array<iv::EventOutputConfig, 1>{{{
                .name = "trigger", .type = iv::EventTypeId::trigger}}};
        }
        void tick(iv::TickSampleContext<TriggerSource> const&) const {}
    };

    struct TriggerSink {
        static constexpr auto event_inputs()
        {
            return std::array<iv::EventInputConfig, 1>{{{
                .name = "gate", .type = iv::EventTypeId::trigger}}};
        }
        void tick(iv::TickSampleContext<TriggerSink> const&) const {}
    };

    void token_provenance_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto frequency = g.input(440.0);
        auto saw = details::configure_concrete_node<SawOscillator>(g);
        saw("frequency"_P = frequency);
        auto out = saw * 0.5f;
        auto trigger = details::configure_concrete_node<TriggerSource>(g).event_port();
        auto event_sink = details::configure_concrete_node<TriggerSink>(g);
        event_sink("gate"_F = trigger);
        g.outputs("main"_P = out);
        g.event_outputs("trigger"_F = trigger);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();
    app.introspection.set_public_sample_outputs(app.graph_input_lanes.public_sample_outputs());
    app.introspection.set_public_event_outputs(app.graph_input_lanes.public_event_outputs());

    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    auto const all = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 1, .column = 1}, .end = {.line = 100, .column = 1}}});

    auto const saw = std::ranges::find_if(all.nodes, [](auto const& node) {
        return node.kind.contains("SawOscillator")
            && node.type_identity != "sample-port";
    });
    ASSERT_NE(saw, all.nodes.end());
    ASSERT_FALSE(saw->source_spans.empty());
    for (auto const& span : saw->source_spans) {
        auto const text = source_text(span);
        EXPECT_EQ(text, "saw");
    }
    EXPECT_TRUE(std::ranges::any_of(saw->source_spans, [](auto const& span) {
        return source_text(span) == "saw";
    }));

    auto const public_input = std::ranges::find_if(all.nodes, [](auto const& node) {
        return node.kind == "Public input";
    });
    ASSERT_NE(public_input, all.nodes.end());
    ASSERT_FALSE(public_input->source_spans.empty());
    for (auto const& span : public_input->source_spans)
        EXPECT_EQ(source_text(span), "frequency");

    auto const sample_output = std::ranges::find_if(all.nodes, [](auto const& node) {
        return node.kind == "Public output" && !node.sample_outputs.empty();
    });
    ASSERT_NE(sample_output, all.nodes.end());
    ASSERT_EQ(sample_output->source_spans.size(), 1u);
    EXPECT_EQ(source_text(sample_output->source_spans.front()), "\"main\"");

    auto const event_output = std::ranges::find_if(all.nodes, [](auto const& node) {
        return node.kind == "Public event output" && !node.event_outputs.empty();
    });
    ASSERT_NE(event_output, all.nodes.end());
    ASSERT_EQ(event_output->source_spans.size(), 1u);
    EXPECT_EQ(source_text(event_output->source_spans.front()), "\"trigger\"");

    auto const source_map = iv::SourceTextLineMap::from_file(module_cpp);
    auto query_at = [&](std::string_view needle, size_t inside = 0) {
        auto const offset = source_map.text.find(needle);
        EXPECT_NE(offset, std::string::npos) << needle;
        auto const position = source_map.position_for(offset + inside);
        return app.query_by_spans(
            module_cpp,
            {{.start = position, .end = position}},
            iv::SourceRangeMatchMode::intersection);
    };
    auto has_kind = [](iv::ProjectQueryResult const& result, std::string_view kind) {
        return std::ranges::any_of(result.nodes, [&](auto const& node) {
            return node.kind.contains(kind);
        });
    };

    // Direct builder/node expressions are not source-active anymore.
    EXPECT_TRUE(query_at("g.input", 2).nodes.empty());
    EXPECT_TRUE(query_at("configure_concrete_node<SawOscillator>", 5).nodes.empty());
    EXPECT_TRUE(query_at("g.outputs", 2).nodes.empty());

    // The name string is active, but the UDL suffix is not.
    auto const frequency_port = query_at("\"frequency\"_P", 2);
    auto const frequency_node = std::ranges::find_if(
        frequency_port.nodes,
        [](auto const& node) { return node.kind.contains("SawOscillator"); });
    ASSERT_NE(frequency_node, frequency_port.nodes.end());
    ASSERT_EQ(frequency_node->sample_inputs.size(), 1u);
    EXPECT_EQ(frequency_node->sample_inputs.front().name, "frequency");
    EXPECT_TRUE(frequency_node->sample_outputs.empty());
    EXPECT_TRUE(frequency_node->event_inputs.empty());
    EXPECT_TRUE(frequency_node->event_outputs.empty());
    ASSERT_EQ(frequency_node->source_spans.size(), 1u);
    EXPECT_EQ(source_text(frequency_node->source_spans.front()), "\"frequency\"");
    EXPECT_TRUE(has_kind(query_at("\"frequency\"_P", std::string_view("\"frequency\"").size()), "SawOscillator"));
    auto const gate_port = query_at("\"gate\"_F", 2);
    auto const gate_node = std::ranges::find_if(
        gate_port.nodes,
        [](auto const& node) { return node.kind.contains("TriggerSink"); });
    ASSERT_NE(gate_node, gate_port.nodes.end());
    ASSERT_EQ(gate_node->event_inputs.size(), 1u);
    EXPECT_EQ(gate_node->event_inputs.front().name, "gate");
    EXPECT_TRUE(gate_node->sample_inputs.empty());
    EXPECT_TRUE(gate_node->sample_outputs.empty());
    EXPECT_TRUE(gate_node->event_outputs.empty());
    ASSERT_EQ(gate_node->source_spans.size(), 1u);
    EXPECT_EQ(source_text(gate_node->source_spans.front()), "\"gate\"");
    EXPECT_TRUE(has_kind(
        query_at("\"gate\"_F", std::string_view("\"gate\"").size()),
        "TriggerSink"));
    EXPECT_TRUE(has_kind(query_at("frequency);", 2), "Public input"));
    EXPECT_TRUE(std::ranges::any_of(query_at("\"main\"_P", 2).nodes, [](auto const& node) {
        return node.kind == "Public output";
    }));
    EXPECT_TRUE(std::ranges::any_of(query_at("\"trigger\"_F", 2).nodes, [](auto const& node) {
        return node.kind == "Public event output";
    }));
}

TEST(IvModuleSourceIntrospection, QueryBySpansKeepsAnnotatedVirtualNodeIdStableAcrossReload)
{
    auto const workspace = make_inline_module_workspace(
        "iv_module_source_introspection_stable_annotated_id",
        R"(#include <intravenous/dsl.h>

namespace {
    void annotated_symbol_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const a = _annotate_node_source_info(
            details::configure_concrete_node<Constant>(g, 0.0f).node_ref(),
            "decl:annotated_symbol_module::a"
        );
        auto const& sink = a;
        g.outputs("main"_P = sink);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    auto const initial = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 1, .column = 1}, .end = {.line = 24, .column = 1}}});

    auto const initial_it = std::find_if(initial.nodes.begin(), initial.nodes.end(), [](auto const &node) {
        return node.kind.contains("Constant");
    });
    ASSERT_NE(initial_it, initial.nodes.end());
    auto const initial_id = initial_it->id;
    ASSERT_FALSE(initial_id.empty());

    auto const original_text = iv::test::read_text(module_cpp);
    iv::test::write_text(module_cpp, original_text + "\n");
    std::this_thread::sleep_for(1s);
    auto const reloaded = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 1, .column = 1}, .end = {.line = 25, .column = 1}}});
    iv::test::write_text(module_cpp, original_text);

    auto const reloaded_it = std::find_if(reloaded.nodes.begin(), reloaded.nodes.end(), [](auto const &node) {
        return node.kind.contains("Constant");
    });
    ASSERT_NE(reloaded_it, reloaded.nodes.end());
    EXPECT_EQ(reloaded_it->id, initial_id);
}

TEST(IvModuleSourceIntrospection, QueryBySpansReturnsAnnotatedVirtualNode)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_annotated_symbol",
        R"(#include <intravenous/dsl.h>

namespace {
    void annotated_symbol_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const a = _annotate_node_source_info(
            details::configure_concrete_node<Constant>(g, 0.0f).node_ref(),
            "decl:annotated_symbol_module::a"
        );
        auto const& sink = a;
        g.outputs("main"_P = sink);
    }
}

)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 24, .column = 1}}});

    auto const it = std::find_if(result.nodes.begin(), result.nodes.end(), [](auto const &node) {
        return node.kind.contains("Constant");
    });
    ASSERT_NE(it, result.nodes.end());
    EXPECT_FALSE(it->id.empty());
    EXPECT_FALSE(it->source_spans.empty());
}

TEST(IvModuleSourceIntrospection, AnnotatesTiledSampleValueAndAggregateNodeCall)
{
    auto const workspace = make_inline_module_workspace(
        "iv_module_source_introspection_tiled_value_and_node_call",
        R"(#include <intravenous/dsl.h>

namespace {
    struct TriggerSource
    {
        static constexpr auto event_outputs()
        {
            return std::array<iv::EventOutputConfig, 1>{{{
                .name = "trigger", .type = iv::EventTypeId::trigger}}};
        }
        void tick(iv::TickSampleContext<TriggerSource> const&) const {}
    };

    void tiled_value_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const left = details::configure_concrete_node<Constant>(g, 0.25f);
        auto const right = details::configure_concrete_node<Constant>(g, -0.25f);
        auto const p = g.tile<stereo>(left, right);
        auto const trigger = details::configure_concrete_node<TriggerSource>(g).event_port();
        auto const sink = details::configure_concrete_node<
            Sum<stereo, SampleStreamLayout::planar, 1>>(g);
        sink(p);
        g.outputs(sink);
        g.event_outputs(trigger);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(
        workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const module_cpp =
        std::filesystem::weakly_canonical(workspace / "module.cpp");
    auto const tiled_value = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 19, .column = 1},
          .end = {.line = 19, .column = 80}}});
    auto const port = std::find_if(
        tiled_value.nodes.begin(), tiled_value.nodes.end(),
        [](auto const& node) { return node.type_identity == "sample-port"; });
    ASSERT_NE(port, tiled_value.nodes.end());
    ASSERT_EQ(port->sample_outputs.size(), 1u);
    EXPECT_EQ(
        port->sample_outputs.front().sample_channel_type,
        iv::ChannelTypeId::stereo);
    ASSERT_EQ(port->members.size(), 1u);
    for (auto const& member : port->members)
        EXPECT_EQ(member.sample_outputs.size(), 1u);

    auto const event_value = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 20, .column = 1},
          .end = {.line = 20, .column = 80}}});
    auto const event_port = std::find_if(
        event_value.nodes.begin(), event_value.nodes.end(),
        [](auto const& node) { return node.type_identity == "event-port"; });
    ASSERT_NE(event_port, event_value.nodes.end());
    ASSERT_EQ(event_port->event_outputs.size(), 1u);
    ASSERT_EQ(event_port->members.size(), 1u);
    EXPECT_EQ(event_port->members.front().event_outputs.size(), 1u);

    auto const direct_initializer = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 22, .column = 1},
          .end = {.line = 22, .column = 40}}});
    EXPECT_FALSE(std::ranges::any_of(
        direct_initializer.nodes,
        [](auto const& node) {
            return node.kind.contains("Sum")
                && node.type_identity != "sample-port";
        }));

    auto const node_call = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 23, .column = 1},
          .end = {.line = 23, .column = 40}}});
    auto const sink = std::find_if(
        node_call.nodes.begin(), node_call.nodes.end(),
        [](auto const& node) {
            return node.kind.contains("Sum")
                && node.type_identity != "sample-port";
        });
    ASSERT_NE(sink, node_call.nodes.end());
    EXPECT_FALSE(sink->source_spans.empty());
}

TEST(IvModuleSourceIntrospection, QueryBySpansReturnsSingleAssignedDeclarationBackedRef)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_single_assigned_ref",
        R"(#include <intravenous/dsl.h>

namespace {
    void assigned_ref_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        NodeRef x;
        x = details::configure_concrete_node<Constant>(g, 0.0f).node_ref();
        auto const& sink = x;
        g.outputs("main"_P = sink);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 24, .column = 1}}});

    auto const it = std::find_if(result.nodes.begin(), result.nodes.end(), [](auto const &node) {
        return node.kind.contains("Constant");
    });
    ASSERT_NE(it, result.nodes.end());
    EXPECT_FALSE(it->source_spans.empty());
}

TEST(IvModuleSourceIntrospection, InitializationFailsWhenDeclarationBackedRefIsAssignedTwice)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_double_assignment_fails",
        R"(#include <intravenous/dsl.h>

namespace {
    void assigned_twice_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        NodeRef x;
        x = details::configure_concrete_node<Constant>(g, 0.0f).node_ref();
        x = details::configure_concrete_node<Constant>(g, 1.0f).node_ref();
        auto const& sink = x;
        g.outputs("main"_P = sink);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    EXPECT_THROW((void)app.initialize(), std::exception);
}

TEST(IvModuleSourceIntrospection, QueryBySpansDoesNotMergeDifferentSchemas)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_schema_mismatch",
        R"(#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/arithmetic.h>

namespace {
    template<size_t Inputs>
    iv::NodeRef make_sum(iv::GraphBuilder& g)
    {
        return iv::details::configure_concrete_node<
            iv::Sum<iv::mono, iv::SampleStreamLayout::planar, Inputs>>(g).node_ref();
    }

    void schema_mismatch_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const a = make_sum<2>(g);
        auto const b = make_sum<3>(g);
        auto const sink = a + b;
        g.outputs("main"_P = sink);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 21, .column = 1}}});

    size_t singleton_sum_count = 0;
    for (auto const &node : result.nodes) {
        if (node.member_count == 1 && node.kind.contains("Sum<") &&
            (node.sample_inputs.size() == 2 || node.sample_inputs.size() == 3)) {
            ++singleton_sum_count;
        }
    }
    EXPECT_GE(singleton_sum_count, 2u);
}

TEST(IvModuleSourceIntrospection, SameLvalueWithDifferentNodeTypesProducesIndependentVirtualNodes)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_split_lvalue_types",
        R"(#include <intravenous/dsl.h>

namespace {
    void split_lvalue_types_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto make_branch = [&]<bool Add>(auto output) {
            auto const value = details::configure_concrete_node<Constant>(g, 1.0f);
            NodeRef p;
            if constexpr (Add) {
                p = value + 1.0f;
            } else {
                p = value - 1.0f;
            }
            g.outputs(output = p);
        };

        make_branch.template operator()<true>("first_sum"_P);
        make_branch.template operator()<false>("difference"_P);
        make_branch.template operator()<true>("second_sum"_P);
    }
}
)"
    );

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        { { .start = { .line = 1, .column = 1 }, .end = { .line = 28, .column = 1 } } }
    );

    std::vector<iv::VirtualNodeInfo const*> split_nodes;
    for (auto const& node : result.nodes) {
        if (node.source_identity.ends_with("@p"))
            split_nodes.push_back(&node);
    }
    ASSERT_EQ(split_nodes.size(), 2u);
    EXPECT_EQ(split_nodes[0]->source_identity, split_nodes[1]->source_identity);
    EXPECT_NE(split_nodes[0]->id, split_nodes[1]->id);
    EXPECT_NE(split_nodes[0]->type_identity, split_nodes[1]->type_identity);
    EXPECT_TRUE(split_nodes[0]->id.contains("#type:"));
    EXPECT_TRUE(split_nodes[1]->id.contains("#type:"));

    auto const sum = std::ranges::find_if(split_nodes, [](auto const* node) {
        return node->kind.contains("Sum<");
    });
    auto const difference = std::ranges::find_if(split_nodes, [](auto const* node) {
        return node->kind.contains("BinaryOpNode");
    });
    ASSERT_NE(sum, split_nodes.end());
    ASSERT_NE(difference, split_nodes.end());
    ASSERT_EQ((*sum)->members.size(), 2u);
    EXPECT_EQ((*sum)->members[0].ordinal, 0u);
    EXPECT_EQ((*sum)->members[1].ordinal, 1u);
    ASSERT_EQ((*difference)->members.size(), 1u);
    EXPECT_EQ((*difference)->members[0].ordinal, 0u);

    app.set_sample_input_value((*sum)->id, 0, 0.25f);
    auto const updated_sum = app.get_virtual_node((*sum)->id);
    auto const untouched_difference = app.get_virtual_node((*difference)->id);
    EXPECT_FLOAT_EQ(static_cast<float>(updated_sum.sample_inputs[0].current_value), 0.25f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(untouched_difference.sample_inputs[0].current_value),
        0.0f
    );
}

TEST(IvModuleSourceIntrospection, QueryBySpansAggregatesMixedConnectivity)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_mixed_connectivity",
        R"(#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/arithmetic.h>

namespace {
    template<int I>
    iv::NodeRef make_sum(iv::GraphBuilder& g)
    {
        (void)I;
        return iv::details::configure_concrete_node<
            iv::Sum<iv::mono, iv::SampleStreamLayout::planar, 1>>(g).node_ref();
    }

    void mixed_connectivity_module(iv::GraphBuilder& g)
    {
        using namespace iv;
        auto const value = details::configure_concrete_node<iv::Constant>(
            g, 0.0f).node_ref();
        auto const a = make_sum<0>(g);
        auto const b = make_sum<1>(g);
        a(value);
        auto const sink = a + b;
        g.outputs("main"_P = sink);
    }
}
)");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {{.start = {.line = 1, .column = 1}, .end = {.line = 24, .column = 1}}});

    size_t connected_sum_count = 0;
    size_t disconnected_sum_count = 0;
    for (auto const &node : result.nodes) {
        if (!node.kind.contains("Sum<") || node.sample_inputs.size() != 1) continue;
        EXPECT_EQ(node.member_count, 1u);
        if (node.sample_inputs.front().connectivity == iv::VirtualPortConnectivity::connected) {
            ++connected_sum_count;
        } else if (node.sample_inputs.front().connectivity == iv::VirtualPortConnectivity::disconnected) {
            ++disconnected_sum_count;
        }
    }
    EXPECT_EQ(connected_sum_count, 1u);
    EXPECT_EQ(disconnected_sum_count, 1u);
}

TEST(IvModuleSourceIntrospection, QueryBySpansIntersectsMultipleSelections)
{
    auto const workspace = read_only_module_fixture_workspace("local_cmake");
    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const tone_range = iv::SourceRange{.start = {.line = 8, .column = 20}, .end = {.line = 8, .column = 20}};
    auto const frequency_range = iv::SourceRange{.start = {.line = 11, .column = 24}, .end = {.line = 11, .column = 24}};

    auto const tone_only = app.query_by_spans(module_cpp, {tone_range}, iv::SourceRangeMatchMode::intersection);
    auto const frequency_only = app.query_by_spans(module_cpp, {frequency_range}, iv::SourceRangeMatchMode::intersection);
    auto const both = app.query_by_spans(
        module_cpp, {tone_range, frequency_range}, iv::SourceRangeMatchMode::intersection);

    auto const ids = [](iv::ProjectQueryResult const &query) {
        std::set<std::string> node_ids;
        for (auto const &node : query.nodes) node_ids.insert(node.id);
        return node_ids;
    };

    auto const tone_ids = ids(tone_only);
    auto const frequency_ids = ids(frequency_only);
    auto const both_ids = ids(both);

    std::set<std::string> intersection;
    std::set_intersection(
        tone_ids.begin(), tone_ids.end(),
        frequency_ids.begin(), frequency_ids.end(),
        std::inserter(intersection, intersection.end()));
    EXPECT_EQ(both_ids, intersection);
}

TEST(IvModuleSourceIntrospection, QueryBySpansUnionsMultipleSelections)
{
    auto const workspace = read_only_module_fixture_workspace("local_cmake");
    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const tone_range = iv::SourceRange{.start = {.line = 8, .column = 20}, .end = {.line = 8, .column = 20}};
    auto const frequency_range = iv::SourceRange{.start = {.line = 11, .column = 24}, .end = {.line = 11, .column = 24}};

    auto const tone_only = app.query_by_spans(module_cpp, {tone_range}, iv::SourceRangeMatchMode::intersection);
    auto const frequency_only = app.query_by_spans(module_cpp, {frequency_range}, iv::SourceRangeMatchMode::intersection);
    auto const both = app.query_by_spans(
        module_cpp, {tone_range, frequency_range}, iv::SourceRangeMatchMode::union_);

    auto const ids = [](iv::ProjectQueryResult const &query) {
        std::set<std::string> node_ids;
        for (auto const &node : query.nodes) node_ids.insert(node.id);
        return node_ids;
    };

    auto const tone_ids = ids(tone_only);
    auto const frequency_ids = ids(frequency_only);
    auto const both_ids = ids(both);

    std::set<std::string> expected_union = tone_ids;
    expected_union.insert(frequency_ids.begin(), frequency_ids.end());
    EXPECT_EQ(both_ids, expected_union);
}

TEST(IvModuleSourceIntrospection, QueryActiveRegionsReturnsOnlySourceSpans)
{
    auto const workspace = read_only_module_fixture_workspace("local_cmake");
    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const nodes = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 1, .column = 1}, .end = {.line = 1000, .column = 1}}},
        iv::SourceRangeMatchMode::intersection);
    auto const active_regions = app.query_active_regions(module_cpp);

    auto const span_key = [](iv::LiveSourceSpan const &span) {
        return span.file_path + ":" + std::to_string(span.range.start.line) + ":" +
            std::to_string(span.range.start.column) + ":" + std::to_string(span.range.end.line) +
            ":" + std::to_string(span.range.end.column);
    };

    std::set<std::string> expected_spans;
    for (auto const &node : nodes.nodes) {
        for (auto const &span : node.source_spans) expected_spans.insert(span_key(span));
    }

    std::set<std::string> actual_spans;
    for (auto const &span : active_regions.source_spans) {
        actual_spans.insert(span_key(span));
        auto const query = app.query_by_spans(
            module_cpp, {span.range}, iv::SourceRangeMatchMode::intersection);
        EXPECT_FALSE(query.nodes.empty()) << span_key(span);
    }
    for (auto const& expected : expected_spans) {
        EXPECT_TRUE(actual_spans.contains(expected)) << expected;
    }
}

TEST(IvModuleSourceIntrospection, QueryBySpansMergesPolyphonicCallbackNodesByExactSourceSpan)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_polyphonic_exact_spans",
        R"(#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/polyphonic.h>
#include <intravenous/basic_nodes/shaping.h>

void polyphonic_module(iv::GraphBuilder& g)
{
    using namespace iv;



    iv::polyphonic<2>(g, [&]<size_t Voice>(auto m) {
        auto const saw = details::configure_concrete_node<SawOscillator>(g);
        saw(
            "phase_offset"_P = 0.0,
            "frequency"_P = 440.0
        );
        (void)Voice;
        g.outputs("main"_P = saw * m["amplitude"_P]);
    });
}
)");

    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 12, .column = 20}, .end = {.line = 12, .column = 20}}});

    ASSERT_EQ(result.nodes.size(), 1u);
    auto const& virtual_node = result.nodes.front();
    EXPECT_EQ(virtual_node.kind, "iv::SawOscillator");
    EXPECT_EQ(virtual_node.member_count, 2u);
    ASSERT_EQ(virtual_node.members.size(), 2u);
    EXPECT_EQ(virtual_node.members[0].ordinal, 0u);
    EXPECT_EQ(virtual_node.members[1].ordinal, 1u);
    EXPECT_EQ(virtual_node.members[0].kind, "iv::SawOscillator");
    EXPECT_EQ(virtual_node.members[1].kind, "iv::SawOscillator");
    ASSERT_EQ(virtual_node.sample_inputs.size(), 2u);
    EXPECT_EQ(virtual_node.sample_inputs[0].name, "phase_offset");
    EXPECT_EQ(virtual_node.sample_inputs[1].name, "frequency");
    ASSERT_EQ(virtual_node.sample_outputs.size(), 1u);
    EXPECT_EQ(virtual_node.sample_outputs[0].name, "out");

    auto const resolved = app.get_virtual_node(virtual_node.id);
    EXPECT_EQ(resolved.kind, "iv::SawOscillator");
    EXPECT_EQ(resolved.member_count, 2u);
    ASSERT_EQ(resolved.sample_inputs.size(), 2u);
    EXPECT_EQ(resolved.sample_inputs[0].name, "phase_offset");
    EXPECT_EQ(resolved.sample_inputs[1].name, "frequency");
    ASSERT_EQ(resolved.sample_outputs.size(), 1u);
    EXPECT_EQ(resolved.sample_outputs[0].name, "out");

    app.set_sample_input_value(virtual_node.id, 1, 0.25f);
    auto const virtual_override = app.get_virtual_node(virtual_node.id);
    ASSERT_EQ(virtual_override.members.size(), 2u);
    EXPECT_FLOAT_EQ(static_cast<float>(virtual_override.sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(virtual_override.members[0].sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(virtual_override.members[1].sample_inputs[1].current_value), 0.25f);
    EXPECT_FALSE(virtual_override.members[0].sample_inputs[1].has_concrete_override);
    EXPECT_FALSE(virtual_override.members[1].sample_inputs[1].has_concrete_override);

    app.set_sample_input_value(virtual_node.id, 1, 0.75f, 1u);
    auto const concrete_override = app.get_virtual_node(virtual_node.id);
    ASSERT_EQ(concrete_override.members.size(), 2u);
    EXPECT_FLOAT_EQ(static_cast<float>(concrete_override.sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(concrete_override.members[0].sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(concrete_override.members[1].sample_inputs[1].current_value), 0.75f);
    EXPECT_FALSE(concrete_override.members[0].sample_inputs[1].has_concrete_override);
    EXPECT_TRUE(concrete_override.members[1].sample_inputs[1].has_concrete_override);

    app.set_sample_input_state(
        virtual_node.id,
        1,
        iv::ProjectSampleInputState::virtual_follow,
        1u);
    auto const cleared_override = app.get_virtual_node(virtual_node.id);
    ASSERT_EQ(cleared_override.members.size(), 2u);
    EXPECT_FLOAT_EQ(static_cast<float>(cleared_override.sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(cleared_override.members[0].sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(cleared_override.members[1].sample_inputs[1].current_value), 0.25f);
    EXPECT_FALSE(cleared_override.members[0].sample_inputs[1].has_concrete_override);
    EXPECT_FALSE(cleared_override.members[1].sample_inputs[1].has_concrete_override);

    EXPECT_TRUE(std::ranges::none_of(result.nodes, [](auto const &node) {
        return node.kind == "Polyphonic";
    }));
}

TEST(IvModuleSourceIntrospection, QueryBySpansDoesNotAttributeInteriorPolyphonicLambdaSpansToOuterSubgraph)
{
    auto const workspace = shared_inline_module_workspace(
        "iv_module_source_introspection_polyphonic_interior_span",
        R"(#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/polyphonic.h>
#include <intravenous/basic_nodes/shaping.h>

void polyphonic_module(iv::GraphBuilder& g)
{
    using namespace iv;



    iv::polyphonic<2>(g, [&]<size_t Voice>(auto m) {
        auto const saw = details::configure_concrete_node<SawOscillator>(g);
        saw(
            "phase_offset"_P = 0.0,
            "frequency"_P = 440.0
        );
        (void)Voice;
        g.outputs("main"_P = saw * m["amplitude"_P]);
    });
}
)");

    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const result = app.query_by_spans(
        module_cpp,
        {{.start = {.line = 12, .column = 20}, .end = {.line = 12, .column = 20}}});

    ASSERT_EQ(result.nodes.size(), 1u);
    EXPECT_EQ(result.nodes.front().kind, "iv::SawOscillator");
    EXPECT_TRUE(std::ranges::none_of(result.nodes, [](auto const &node) {
        return node.kind == "Polyphonic";
    }));
    EXPECT_TRUE(std::ranges::none_of(result.nodes, [](auto const &node) {
        return node.kind == "PolyphonicVoice";
    }));
}

TEST(IvModuleSourceIntrospection, ReloadKeepsVirtualNodeIdsAddressable)
{
    auto const workspace = mutable_module_fixture_workspace("iv_module_source_introspection_reload_epoch", "local_cmake");
    auto const module_cpp = workspace / "module.cpp";
    iv::test_support::write_text(workspace / "iv_project.jsonl", "");

    SeededIvModuleSourceIntrospectionApp app(workspace, iv::test::repo_root(), {});
    app.initialize();

    auto const initial = app.query_by_spans(
        std::filesystem::weakly_canonical(module_cpp),
        {{.start = {.line = 7, .column = 1}, .end = {.line = 18, .column = 1}}});
    ASSERT_FALSE(initial.nodes.empty());

    auto const original_text = iv::test::read_text(module_cpp);
    iv::test::write_text(module_cpp, original_text + "\n");
    std::this_thread::sleep_for(1s);
    auto const reloaded = app.query_by_spans(
        std::filesystem::weakly_canonical(module_cpp),
        {{.start = {.line = 7, .column = 1}, .end = {.line = 19, .column = 1}}});
    iv::test::write_text(module_cpp, original_text);

    EXPECT_FALSE(reloaded.nodes.empty());
    EXPECT_NO_THROW((void)app.get_virtual_node(initial.nodes.front().id));
}
