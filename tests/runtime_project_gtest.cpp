#include "module_test_utils.h"
#include "runtime/graph_input_lanes.h"
#include "runtime/graph_input_lanes_lane_views_bridge.h"
#include "runtime/graph_input_lanes_timeline_bridge.h"
#include "runtime/iv_module_definitions.h"
#include "runtime/iv_module_definitions_iv_module_instances_bridge.h"
#include "runtime/iv_module_definitions_project_introspection_bridge.h"
#include "runtime/iv_module_instances.h"
#include "runtime/iv_module_instances_graph_input_lanes_bridge.h"
#include "runtime/lane_views.h"
#include "runtime/lane_views_timeline_bridge.h"
#include "runtime/project_introspection.h"
#include "runtime/project_introspection_graph_input_lanes_bridge.h"
#include "runtime/runtime_project_events.h"
#include "runtime/timeline.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <source_location>
#include <set>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
    using namespace std::chrono_literals;

    std::filesystem::path make_workspace(
        std::string_view name,
        std::source_location location = std::source_location::current()
    )
    {
        return iv::test::fresh_test_workspace(name, location);
    }

    void write_text(std::filesystem::path const& path, std::string const& text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(static_cast<bool>(out)) << "failed to open " << path;
        out << text;
    }

    std::filesystem::path copy_fixture_workspace(
        std::string_view test_name,
        std::string const& fixture_name,
        std::source_location location = std::source_location::current()
    )
    {
        auto const workspace = make_workspace(test_name, location);
        iv::test::copy_directory(iv::test::test_modules_root() / fixture_name, workspace);
        return workspace;
    }

    std::filesystem::path make_inline_module_workspace(
        std::string_view test_name,
        std::string const& module_text,
        std::source_location location = std::source_location::current()
    )
    {
        auto const workspace = make_workspace(test_name, location);
        write_text(workspace / ".intravenous", "");
        write_text(workspace / "module.cpp", module_text);
        return workspace;
    }

    std::string find_program(std::string const& name)
    {
        std::string command = "command -v " + name;
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            return {};
        }

        std::string output;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            output += buffer;
        }
        pclose(pipe);

        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
            output.pop_back();
        }
        return output;
    }

    std::string configured_program_or_find(std::string const& name, char const* configured_path)
    {
        if (configured_path != nullptr && *configured_path != '\0') {
            return configured_path;
        }
        return find_program(name);
    }

    struct ScopedEnvVar {
        std::string key;
        std::optional<std::string> original;

        ScopedEnvVar(std::string key_, std::string value) :
            key(std::move(key_))
        {
            if (char const* existing = std::getenv(key.c_str())) {
                original = existing;
            }
            setenv(key.c_str(), value.c_str(), 1);
        }

        ~ScopedEnvVar()
        {
            if (original.has_value()) {
                setenv(key.c_str(), original->c_str(), 1);
            } else {
                unsetenv(key.c_str());
            }
        }
    };

    struct BoundRuntimeProjectIntrospection {
        iv::Timeline timeline;
        iv::RuntimeIvModuleDefinitions iv_module_definitions;
        iv::RuntimeIvModuleInstances iv_module_instances;
        iv::RuntimeGraphInputLanes graph_input_lanes;
        iv::RuntimeLaneViews lane_views;
        iv::RuntimeProjectIntrospection introspection;

        BoundRuntimeProjectIntrospection(
            std::filesystem::path workspace_root,
            std::filesystem::path discovery_start,
            std::vector<std::filesystem::path> extra_search_roots,
            iv::AudioDeviceFactory audio_device_factory = {}) :
            iv_module_definitions(
                std::move(workspace_root),
                std::move(discovery_start),
                std::vector<std::filesystem::path>(std::move(extra_search_roots)),
                std::move(audio_device_factory))
        {
            iv::bind_graph_input_lanes_timeline_bridge(timeline);
            iv::bind_graph_input_lanes_lane_views_bridge(graph_input_lanes);
            iv::bind_iv_module_definitions_iv_module_instances_bridge(iv_module_instances);
            iv::bind_iv_module_definitions_project_introspection_bridge(introspection);
            iv::bind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
            iv::bind_project_introspection_graph_input_lanes_bridge(graph_input_lanes);
            iv::bind_lane_views_timeline_bridge(lane_views);
        }

        ~BoundRuntimeProjectIntrospection()
        {
            iv::unbind_lane_views_timeline_bridge(lane_views);
            iv::unbind_project_introspection_graph_input_lanes_bridge(graph_input_lanes);
            iv::unbind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
            iv::unbind_iv_module_definitions_project_introspection_bridge(introspection);
            iv::unbind_iv_module_definitions_iv_module_instances_bridge(iv_module_instances);
            iv::unbind_graph_input_lanes_lane_views_bridge(graph_input_lanes);
            iv::unbind_graph_input_lanes_timeline_bridge(timeline);
            iv_module_definitions.request_shutdown();
        }

        auto initialize()
        {
            (void)iv_module_definitions.initialize();
            return introspection.initialize();
        }
        auto query_by_spans(
            std::filesystem::path const& file_path,
            std::vector<iv::SourceRange> const& ranges,
            iv::SourceRangeMatchMode match_mode = iv::SourceRangeMatchMode::intersection) const
        {
            return introspection.query_by_spans(file_path, ranges, match_mode);
        }
        auto query_active_regions(std::filesystem::path const& file_path) const
        {
            return introspection.query_active_regions(file_path);
        }
        auto get_logical_node(std::string const& node_id) const
        {
            return introspection.get_logical_node(node_id);
        }
        auto get_logical_nodes(std::vector<std::string> const& node_ids) const
        {
            return introspection.get_logical_nodes(node_ids);
        }
        auto open_lane_view(iv::LaneViewRequest request)
        {
            return lane_views.open_view(std::move(request));
        }
        auto update_lane_view(iv::LaneViewRequest request)
        {
            return lane_views.update_view(std::move(request));
        }
        void close_lane_view(std::string const& view_id)
        {
            lane_views.close_view(view_id);
        }
        void set_sample_input_value(
            std::string const& node_id,
            size_t input_ordinal,
            iv::Sample value,
            std::optional<size_t> member_ordinal = std::nullopt)
        {
            auto const port = introspection.sample_graph_input_port_for_node(
                node_id,
                member_ordinal,
                input_ordinal);
            graph_input_lanes.set_sample_input_value(
                iv::RuntimeProjectSetSampleInputValueRequest{
                    .node_id = node_id,
                    .member_ordinal = member_ordinal,
                    .input_ordinal = input_ordinal,
                    .value = value,
                    .graph_input_port = port,
                });
        }
        void clear_sample_input_value_override(
            std::string const& node_id,
            size_t member_ordinal,
            size_t input_ordinal)
        {
            auto const port = introspection.sample_graph_input_port_for_node(
                node_id,
                member_ordinal,
                input_ordinal);
            graph_input_lanes.clear_sample_input_value_override(
                iv::RuntimeProjectClearSampleInputValueOverrideRequest{
                    .node_id = node_id,
                    .member_ordinal = member_ordinal,
                    .input_ordinal = input_ordinal,
                    .graph_input_port = port,
                });
        }
    };
}

TEST(RuntimeProjectIntrospection, EmptyIntravenousMarkerUsesWorkspaceRoot)
{
    auto const workspace = copy_fixture_workspace("runtime_project_empty_marker", "local_cmake");
    write_text(workspace / ".intravenous", "");

    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    auto const initialized = service.initialize();

    EXPECT_EQ(initialized.module_root, std::filesystem::weakly_canonical(workspace));
    EXPECT_FALSE(initialized.module_id.empty());
}

TEST(RuntimeProjectIntrospection, RelativeRootModulePathResolvesAgainstWorkspaceRoot)
{
    auto const workspace = make_workspace("runtime_project_relative_root");
    auto const module_root = workspace / "module";
    iv::test::copy_directory(iv::test::test_modules_root() / "local_cmake", module_root);
    write_text(workspace / ".intravenous", "rootModulePath=module\n");

    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    auto const initialized = service.initialize();

    EXPECT_EQ(initialized.module_root, std::filesystem::weakly_canonical(module_root));
}

TEST(RuntimeProjectIntrospection, QueryBySpansReturnsMatchingLiveNodesWithPorts)
{
    auto const workspace = copy_fixture_workspace("runtime_project_query_by_spans", "local_cmake");
    write_text(workspace / ".intravenous", "");

    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    auto const initialized = service.initialize();

    auto const result = service.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {
            iv::SourceRange {
                .start = { .line = 7, .column = 1 },
                .end = { .line = 15, .column = 1 },
            },
        }
    );
    ASSERT_FALSE(result.nodes.empty());

    auto const& node = result.nodes.front();
    EXPECT_FALSE(node.id.empty());
    EXPECT_FALSE(node.kind.empty());
    EXPECT_FALSE(node.source_spans.empty());

    bool has_any_port = !node.sample_inputs.empty() || !node.sample_outputs.empty() || !node.event_inputs.empty() || !node.event_outputs.empty();
    EXPECT_TRUE(has_any_port);
}

TEST(RuntimeProjectArchitecture, IvModuleDefinitionsInitializeAndShutdownWithoutBridges)
{
    auto const workspace = copy_fixture_workspace(
        "runtime_project_architecture_definitions_only", "local_cmake");
    write_text(workspace / ".intravenous", "");

    iv::RuntimeIvModuleDefinitions definitions(
        workspace,
        iv::test::repo_root(),
        {},
        {});

    auto const definition = definitions.initialize();
    EXPECT_FALSE(definition.definition_id.empty());
    EXPECT_FALSE(definition.module_id.empty());
    EXPECT_FALSE(definition.module_root.empty());

    definitions.request_shutdown();
}

TEST(RuntimeProjectArchitecture, DefinitionsAndProjectIntrospectionInitializeAndShutdown)
{
    auto const workspace = copy_fixture_workspace(
        "runtime_project_architecture_project_introspection", "local_cmake");
    write_text(workspace / ".intravenous", "");

    iv::RuntimeIvModuleDefinitions definitions(
        workspace,
        iv::test::repo_root(),
        {},
        {});
    iv::RuntimeProjectIntrospection introspection;
    iv::bind_iv_module_definitions_project_introspection_bridge(introspection);

    auto const definition = definitions.initialize();
    auto const initialized = introspection.initialize();

    EXPECT_EQ(initialized.module_id, definition.module_id);

    iv::unbind_iv_module_definitions_project_introspection_bridge(introspection);
    definitions.request_shutdown();
}

TEST(RuntimeProjectArchitecture, DefinitionsInstancesAndGraphInputLanesInitializeAndShutdown)
{
    auto const workspace = copy_fixture_workspace(
        "runtime_project_architecture_graph_input_lanes", "local_cmake");
    write_text(workspace / ".intravenous", "");

    iv::Timeline timeline;
    iv::RuntimeIvModuleDefinitions definitions(
        workspace,
        iv::test::repo_root(),
        {},
        {});
    iv::RuntimeIvModuleInstances instances;
    iv::RuntimeGraphInputLanes graph_input_lanes;

    iv::bind_graph_input_lanes_timeline_bridge(timeline);
    iv::bind_iv_module_definitions_iv_module_instances_bridge(instances);
    iv::bind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);

    auto const definition = definitions.initialize();
    EXPECT_FALSE(definition.introspection.logical_nodes.empty());

    auto const lanes = graph_input_lanes.query_lanes(
        iv::LaneQueryFilter{ .kind = "graphInputs" });
    EXPECT_GT(lanes.total_lane_count, 0u);

    iv::unbind_iv_module_instances_graph_input_lanes_bridge(graph_input_lanes);
    iv::unbind_iv_module_definitions_iv_module_instances_bridge(instances);
    iv::unbind_graph_input_lanes_timeline_bridge(timeline);
    definitions.request_shutdown();
}

TEST(RuntimeProjectIntrospection, QueryBySpansKeepsDistinctDeclarationsSeparate)
{
    auto const workspace = make_inline_module_workspace(
        "runtime_project_query_by_spans_merged_logical",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"

namespace {
    template<int I>
    iv::NodeRef make_value(iv::GraphBuilder& g, iv::ModuleContext const& context)
    {
        (void)I;
        return g.node<iv::ValueSource>(&context.sample_period()).node_ref();
    }

    void merged_logical_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        auto const a = make_value<0>(g, context);
        auto const b = make_value<1>(g, context);
        auto const sink = context.target_factory().sink(g, 0);
        sink(a + b);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.merged_logical_module", merged_logical_module);
)"
    );

    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    service.initialize();

    auto const result = service.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {
            iv::SourceRange {
                .start = { .line = 1, .column = 1 },
                .end = { .line = 22, .column = 1 },
            },
        }
    );

    size_t value_source_count = 0;
    for (auto const& node : result.nodes) {
        if (!node.kind.contains("ValueSource")) {
            continue;
        }
        ++value_source_count;
        EXPECT_EQ(node.member_count, 1u);
        EXPECT_FALSE(node.source_spans.empty());
    }

    EXPECT_EQ(value_source_count, 2u);
}

TEST(RuntimeProjectIntrospection, QueryBySpansKeepsAnnotatedLogicalNodeIdStableAcrossReload)
{
    auto const workspace = make_inline_module_workspace(
        "runtime_project_query_by_spans_stable_annotated_id",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"

namespace {
    void annotated_symbol_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        auto const a = _annotate_node_source_info(
            g.node<ValueSource>(&context.sample_period()).node_ref(),
            "decl:annotated_symbol_module::a"
        );
        auto const sink = context.target_factory().sink(g, 0);
        sink(a);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.annotated_symbol_module", annotated_symbol_module);
)"
    );

    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    service.initialize();

    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    auto const initial = service.query_by_spans(
        module_cpp,
        {
            iv::SourceRange {
                .start = { .line = 1, .column = 1 },
                .end = { .line = 24, .column = 1 },
            },
        }
    );

    auto const initial_it = std::find_if(initial.nodes.begin(), initial.nodes.end(), [](auto const& node) {
        return node.kind.contains("ValueSource");
    });

    ASSERT_NE(initial_it, initial.nodes.end());
    auto const initial_id = initial_it->id;
    ASSERT_FALSE(initial_id.empty());

    auto const original_text = iv::test::read_text(module_cpp);
    iv::test::write_text(module_cpp, original_text + "\n");

    std::this_thread::sleep_for(1s);
    auto const reloaded = service.query_by_spans(
        module_cpp,
        {
            iv::SourceRange {
                .start = { .line = 1, .column = 1 },
                .end = { .line = 25, .column = 1 },
            },
        }
    );

    iv::test::write_text(module_cpp, original_text);

    auto const reloaded_it = std::find_if(reloaded.nodes.begin(), reloaded.nodes.end(), [](auto const& node) {
        return node.kind.contains("ValueSource");
    });

    ASSERT_NE(reloaded_it, reloaded.nodes.end());
    EXPECT_EQ(reloaded_it->id, initial_id);
}

TEST(RuntimeProjectIntrospection, QueryBySpansReturnsAnnotatedLogicalNode)
{
    auto const workspace = make_inline_module_workspace(
        "runtime_project_query_by_spans_annotated_symbol",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"

namespace {
    void annotated_symbol_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        auto const a = _annotate_node_source_info(
            g.node<ValueSource>(&context.sample_period()).node_ref(),
            "decl:annotated_symbol_module::a"
        );
        auto const sink = context.target_factory().sink(g, 0);
        sink(a);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.annotated_symbol_module", annotated_symbol_module);
)"
    );

    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    service.initialize();

    auto const result = service.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {
            iv::SourceRange {
                .start = { .line = 1, .column = 1 },
                .end = { .line = 24, .column = 1 },
            },
        }
    );

    auto const it = std::find_if(result.nodes.begin(), result.nodes.end(), [](auto const& node) {
        return node.kind.contains("ValueSource");
    });

    ASSERT_NE(it, result.nodes.end());
    EXPECT_FALSE(it->id.empty());
    EXPECT_FALSE(it->source_spans.empty());
}

TEST(RuntimeProjectIntrospection, QueryBySpansReturnsSingleAssignedDeclarationBackedRef)
{
    auto const workspace = make_inline_module_workspace(
        "runtime_project_query_by_spans_single_assigned_ref",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"

namespace {
    void assigned_ref_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        NodeRef x;
        x = g.node<ValueSource>(&context.sample_period()).node_ref();
        auto const sink = context.target_factory().sink(g, 0);
        sink(x);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.assigned_ref_module", assigned_ref_module);
)"
    );

    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    service.initialize();

    auto const result = service.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {
            iv::SourceRange {
                .start = { .line = 1, .column = 1 },
                .end = { .line = 24, .column = 1 },
            },
        }
    );

    auto const it = std::find_if(result.nodes.begin(), result.nodes.end(), [](auto const& node) {
        return node.kind.contains("ValueSource");
    });

    ASSERT_NE(it, result.nodes.end());
    EXPECT_FALSE(it->source_spans.empty());
}

TEST(RuntimeProjectIntrospection, InitializationFailsWhenDeclarationBackedRefIsAssignedTwice)
{
    auto const workspace = make_inline_module_workspace(
        "runtime_project_double_assignment_fails",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"

namespace {
    void assigned_twice_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        NodeRef x;
        x = g.node<ValueSource>(&context.sample_period()).node_ref();
        x = g.node<ValueSource>(&context.sample_period()).node_ref();
        auto const sink = context.target_factory().sink(g, 0);
        sink(x);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.assigned_twice_module", assigned_twice_module);
)"
    );

    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    EXPECT_THROW(
        {
            try {
                (void)service.initialize();
            } catch (std::exception const& e) {
                EXPECT_TRUE(std::string(e.what()).contains("already been initialized"));
                throw;
            }
        },
        std::exception
    );
}

TEST(RuntimeProjectIntrospection, QueryBySpansDoesNotMergeDifferentSchemas)
{
    auto const workspace = make_inline_module_workspace(
        "runtime_project_query_by_spans_schema_mismatch",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/arithmetic.h"

namespace {
    template<size_t Inputs>
    iv::NodeRef make_sum(iv::GraphBuilder& g)
    {
        return g.node<iv::Sum<Inputs>>().node_ref();
    }

    void schema_mismatch_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        auto const a = make_sum<2>(g);
        auto const b = make_sum<3>(g);
        auto const sink = context.target_factory().sink(g, 0);
        sink(a + b);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.schema_mismatch_module", schema_mismatch_module);
)"
    );

    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    service.initialize();

    auto const result = service.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {
            iv::SourceRange {
                .start = { .line = 1, .column = 1 },
                .end = { .line = 21, .column = 1 },
            },
        }
    );

    size_t singleton_sum_count = 0;
    for (auto const& node : result.nodes) {
        if (node.member_count == 1
            && node.kind.contains("BinaryOpNode")
            && (node.sample_inputs.size() == 2 || node.sample_inputs.size() == 3)) {
            ++singleton_sum_count;
        }
    }

    EXPECT_GE(singleton_sum_count, 2u);
}

TEST(RuntimeProjectIntrospection, QueryBySpansAggregatesMixedConnectivity)
{
    auto const workspace = make_inline_module_workspace(
        "runtime_project_query_by_spans_mixed_connectivity",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/arithmetic.h"

namespace {
    template<int I>
    iv::NodeRef make_sum(iv::GraphBuilder& g)
    {
        (void)I;
        return g.node<iv::Sum<1>>().node_ref();
    }

    void mixed_connectivity_module(iv::ModuleContext const& context)
    {
        using namespace iv;
        auto& g = context.builder();
        auto const value = g.node<iv::ValueSource>(&context.sample_period()).node_ref();
        auto const a = make_sum<0>(g);
        auto const b = make_sum<1>(g);
        a(value);
        auto const sink = context.target_factory().sink(g, 0);
        sink(a + b);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.mixed_connectivity_module", mixed_connectivity_module);
)"
    );

    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    service.initialize();

    auto const result = service.query_by_spans(
        std::filesystem::weakly_canonical(workspace / "module.cpp"),
        {
            iv::SourceRange {
                .start = { .line = 1, .column = 1 },
                .end = { .line = 22, .column = 1 },
            },
        }
    );

    size_t connected_sum_count = 0;
    size_t disconnected_sum_count = 0;
    for (auto const& node : result.nodes) {
        if (!node.kind.contains("BinaryOpNode") || node.sample_inputs.size() != 1) {
            continue;
        }
        EXPECT_EQ(node.member_count, 1u);
        if (node.sample_inputs.front().connectivity == iv::LogicalPortConnectivity::connected) {
            ++connected_sum_count;
        } else if (node.sample_inputs.front().connectivity == iv::LogicalPortConnectivity::disconnected) {
            ++disconnected_sum_count;
        }
    }

    EXPECT_EQ(connected_sum_count, 1u);
    EXPECT_EQ(disconnected_sum_count, 1u);
}

TEST(RuntimeProjectIntrospection, QueryBySpansIntersectsMultipleSelections)
{
    auto const workspace = copy_fixture_workspace("runtime_project_query_by_spans_intersection", "local_cmake");
    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    write_text(workspace / ".intravenous", "");

    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    service.initialize();

    auto const dt_range = iv::SourceRange {
        .start = { .line = 8, .column = 20 },
        .end = { .line = 8, .column = 20 },
    };
    auto const sink_range = iv::SourceRange {
        .start = { .line = 11, .column = 24 },
        .end = { .line = 11, .column = 24 },
    };

    auto const dt_only = service.query_by_spans(module_cpp, { dt_range }, iv::SourceRangeMatchMode::intersection);
    auto const sink_only = service.query_by_spans(module_cpp, { sink_range }, iv::SourceRangeMatchMode::intersection);
    auto const both = service.query_by_spans(module_cpp, { dt_range, sink_range }, iv::SourceRangeMatchMode::intersection);

    auto const ids = [](iv::RuntimeProjectQueryResult const& query) {
        std::set<std::string> node_ids;
        for (auto const& node : query.nodes) {
            node_ids.insert(node.id);
        }
        return node_ids;
    };

    auto const dt_ids = ids(dt_only);
    auto const sink_ids = ids(sink_only);
    auto const both_ids = ids(both);

    std::set<std::string> intersection;
    std::set_intersection(
        dt_ids.begin(), dt_ids.end(),
        sink_ids.begin(), sink_ids.end(),
        std::inserter(intersection, intersection.end())
    );
    EXPECT_EQ(both_ids, intersection);
}

TEST(RuntimeProjectIntrospection, QueryBySpansUnionsMultipleSelections)
{
    auto const workspace = copy_fixture_workspace("runtime_project_query_by_spans_union", "local_cmake");
    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    write_text(workspace / ".intravenous", "");

    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    service.initialize();

    auto const dt_range = iv::SourceRange {
        .start = { .line = 8, .column = 20 },
        .end = { .line = 8, .column = 20 },
    };
    auto const sink_range = iv::SourceRange {
        .start = { .line = 11, .column = 24 },
        .end = { .line = 11, .column = 24 },
    };

    auto const dt_only = service.query_by_spans(module_cpp, { dt_range }, iv::SourceRangeMatchMode::intersection);
    auto const sink_only = service.query_by_spans(module_cpp, { sink_range }, iv::SourceRangeMatchMode::intersection);
    auto const both = service.query_by_spans(module_cpp, { dt_range, sink_range }, iv::SourceRangeMatchMode::union_);

    auto const ids = [](iv::RuntimeProjectQueryResult const& query) {
        std::set<std::string> node_ids;
        for (auto const& node : query.nodes) {
            node_ids.insert(node.id);
        }
        return node_ids;
    };

    auto const dt_ids = ids(dt_only);
    auto const sink_ids = ids(sink_only);
    auto const both_ids = ids(both);

    std::set<std::string> expected_union = dt_ids;
    expected_union.insert(sink_ids.begin(), sink_ids.end());
    EXPECT_EQ(both_ids, expected_union);
}

TEST(RuntimeProjectIntrospection, QueryActiveRegionsReturnsOnlySourceSpans)
{
    auto const workspace = copy_fixture_workspace("runtime_project_query_active_regions", "local_cmake");
    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    write_text(workspace / ".intravenous", "");

    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    service.initialize();

    auto const nodes = service.query_by_spans(module_cpp, {
        iv::SourceRange {
            .start = { .line = 1, .column = 1 },
            .end = { .line = 1000, .column = 1 },
        }
    }, iv::SourceRangeMatchMode::intersection);
    auto const active_regions = service.query_active_regions(module_cpp);

    auto const span_key = [](iv::LiveSourceSpan const& span) {
        return span.file_path + ":" +
            std::to_string(span.range.start.line) + ":" +
            std::to_string(span.range.start.column) + ":" +
            std::to_string(span.range.end.line) + ":" +
            std::to_string(span.range.end.column);
    };

    std::set<std::string> expected_spans;
    for (auto const& node : nodes.nodes) {
        for (auto const& span : node.source_spans) {
            expected_spans.insert(span_key(span));
        }
    }

    std::set<std::string> actual_spans;
    for (auto const& span : active_regions.source_spans) {
        actual_spans.insert(span_key(span));
    }
    EXPECT_EQ(actual_spans, expected_spans);
}

TEST(RuntimeProjectIntrospection, QueryBySpansMergesPolyphonicCallbackNodesByExactSourceSpan)
{
    auto const workspace = make_inline_module_workspace(
        "runtime_project_polyphonic_exact_spans",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/shaping.h"

void polyphonic_module(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const& io = context.target_factory();
    auto const dt = g.node<ValueSource>(&context.sample_period());
    auto const sink = io.sink(g, 0);
    auto const voices = iv::polyphonic<2>(g, [&](auto m) {
        auto const saw = g.node<SawOscillator>();
        saw(
            "phase_offset"_P = 0.0,
            "frequency"_P = 440.0,
            "dt"_P = dt
        );
        return saw * ("amplitude"_P << m);
    });
    sink(voices);
    g.outputs();
}

IV_EXPORT_MODULE("iv.test.polyphonic_module", polyphonic_module);
)"
    );

    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    service.initialize();

    auto const result = service.query_by_spans(
        module_cpp,
        {
            iv::SourceRange {
                .start = { .line = 13, .column = 20 },
                .end = { .line = 13, .column = 20 },
            },
        }
    );

    ASSERT_EQ(result.nodes.size(), 1u);
    auto const& logical = result.nodes.front();
    EXPECT_EQ(logical.kind, "iv::SawOscillator");
    EXPECT_EQ(logical.member_count, 2u);
    ASSERT_EQ(logical.members.size(), 2u);
    EXPECT_EQ(logical.members[0].ordinal, 0u);
    EXPECT_EQ(logical.members[1].ordinal, 1u);
    EXPECT_EQ(logical.members[0].kind, "iv::SawOscillator");
    EXPECT_EQ(logical.members[1].kind, "iv::SawOscillator");
    ASSERT_EQ(logical.sample_inputs.size(), 3u);
    EXPECT_EQ(logical.sample_inputs[0].name, "phase_offset");
    EXPECT_EQ(logical.sample_inputs[1].name, "frequency");
    EXPECT_EQ(logical.sample_inputs[2].name, "dt");
    ASSERT_EQ(logical.sample_outputs.size(), 1u);
    EXPECT_EQ(logical.sample_outputs[0].name, "out");

    auto const resolved = service.get_logical_node(logical.id);
    EXPECT_EQ(resolved.kind, "iv::SawOscillator");
    EXPECT_EQ(resolved.member_count, 2u);
    ASSERT_EQ(resolved.sample_inputs.size(), 3u);
    EXPECT_EQ(resolved.sample_inputs[0].name, "phase_offset");
    EXPECT_EQ(resolved.sample_inputs[1].name, "frequency");
    EXPECT_EQ(resolved.sample_inputs[2].name, "dt");
    ASSERT_EQ(resolved.sample_outputs.size(), 1u);
    EXPECT_EQ(resolved.sample_outputs[0].name, "out");

    service.set_sample_input_value(logical.id, 1, 0.25f);
    auto const logical_override = service.get_logical_node(logical.id);
    ASSERT_EQ(logical_override.members.size(), 2u);
    EXPECT_FLOAT_EQ(static_cast<float>(logical_override.sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(logical_override.members[0].sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(logical_override.members[1].sample_inputs[1].current_value), 0.25f);
    EXPECT_FALSE(logical_override.members[0].sample_inputs[1].has_concrete_override);
    EXPECT_FALSE(logical_override.members[1].sample_inputs[1].has_concrete_override);

    service.set_sample_input_value(logical.id, 1, 0.75f, 1u);
    auto const concrete_override = service.get_logical_node(logical.id);
    ASSERT_EQ(concrete_override.members.size(), 2u);
    EXPECT_FLOAT_EQ(static_cast<float>(concrete_override.sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(concrete_override.members[0].sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(concrete_override.members[1].sample_inputs[1].current_value), 0.75f);
    EXPECT_FALSE(concrete_override.members[0].sample_inputs[1].has_concrete_override);
    EXPECT_TRUE(concrete_override.members[1].sample_inputs[1].has_concrete_override);

    service.clear_sample_input_value_override(logical.id, 1u, 1);
    auto const cleared_override = service.get_logical_node(logical.id);
    ASSERT_EQ(cleared_override.members.size(), 2u);
    EXPECT_FLOAT_EQ(static_cast<float>(cleared_override.sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(cleared_override.members[0].sample_inputs[1].current_value), 0.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(cleared_override.members[1].sample_inputs[1].current_value), 0.25f);
    EXPECT_FALSE(cleared_override.members[0].sample_inputs[1].has_concrete_override);
    EXPECT_FALSE(cleared_override.members[1].sample_inputs[1].has_concrete_override);

    EXPECT_TRUE(std::ranges::none_of(result.nodes, [](auto const& node) {
        return node.kind == "Polyphonic";
    }));
}

TEST(RuntimeProjectIntrospection, QueryBySpansDoesNotAttributeInteriorPolyphonicLambdaSpansToOuterSubgraph)
{
    auto const workspace = make_inline_module_workspace(
        "runtime_project_polyphonic_interior_span",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/shaping.h"

void polyphonic_module(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const& io = context.target_factory();
    auto const dt = g.node<ValueSource>(&context.sample_period());
    auto const sink = io.sink(g, 0);
    auto const voices = iv::polyphonic<2>(g, [&](auto m) {
        auto const saw = g.node<SawOscillator>();
        saw(
            "phase_offset"_P = 0.0,
            "frequency"_P = 440.0,
            "dt"_P = dt
        );
        return saw * ("amplitude"_P << m);
    });
    sink(voices);
    g.outputs();
}

IV_EXPORT_MODULE("iv.test.polyphonic_module", polyphonic_module);
)"
    );

    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    service.initialize();

    auto const result = service.query_by_spans(
        module_cpp,
        {
            iv::SourceRange {
                .start = { .line = 13, .column = 20 },
                .end = { .line = 13, .column = 20 },
            },
        }
    );

    ASSERT_EQ(result.nodes.size(), 1u);
    EXPECT_EQ(result.nodes.front().kind, "iv::SawOscillator");
    EXPECT_TRUE(std::ranges::none_of(result.nodes, [](auto const& node) {
        return node.kind == "Polyphonic";
    }));
    EXPECT_TRUE(std::ranges::none_of(result.nodes, [](auto const& node) {
        return node.kind == "PolyphonicVoice";
    }));
}

TEST(RuntimeProjectIntrospection, QueryBySpansReturnsPolyphonicOuterLogicalIdentityAtDeclarationSpan)
{
    auto const workspace = make_inline_module_workspace(
        "runtime_project_polyphonic_outer_identity",
        R"(#include "dsl.h"
#include "basic_nodes/buffers.h"
#include "basic_nodes/shaping.h"

void polyphonic_module(iv::ModuleContext const& context)
{
    using namespace iv;
    auto& g = context.builder();
    auto const& io = context.target_factory();
    auto const dt = g.node<ValueSource>(&context.sample_period());
    auto const sink = io.sink(g, 0);
    auto voice = iv::polyphonic<2>(g, [&](auto m) {
        auto const saw = g.node<SawOscillator>();
        saw(
            "phase_offset"_P = 0.0,
            "frequency"_P = 440.0,
            "dt"_P = dt
        );
        return saw * ("amplitude"_P << m);
    });
    auto x = std::move(voice);
    sink(x);
    g.outputs();
}

IV_EXPORT_MODULE("iv.test.polyphonic_module", polyphonic_module);
)"
    );

    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    service.initialize();

    auto const result = service.query_by_spans(
        module_cpp,
        {
            iv::SourceRange {
                .start = { .line = 12, .column = 6 },
                .end = { .line = 12, .column = 10 },
            },
        }
    );

    ASSERT_FALSE(result.nodes.empty());
    EXPECT_EQ(result.nodes.front().kind, "Polyphonic");
    EXPECT_FALSE(result.nodes.front().source_identity.empty());
    EXPECT_TRUE(result.nodes.front().source_identity.contains("@voice")) << result.nodes.front().source_identity;
}

TEST(RuntimeProjectIntrospection, MissingMarkerFailsInitialization)
{
    auto const workspace = copy_fixture_workspace("runtime_project_missing_marker", "local_cmake");

    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    EXPECT_THROW(
        {
            try {
                (void)service.initialize();
            } catch (std::exception const& e) {
                EXPECT_TRUE(std::string(e.what()).contains(".intravenous"));
                throw;
            }
        },
        std::exception
    );
}

TEST(RuntimeProjectIntrospection, ReloadKeepsLogicalNodeIdsAddressable)
{
    auto const workspace = copy_fixture_workspace("runtime_project_reload_epoch", "local_cmake");
    auto const module_cpp = workspace / "module.cpp";
    write_text(workspace / ".intravenous", "");

    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    service.initialize();

    auto const initial = service.query_by_spans(
        std::filesystem::weakly_canonical(module_cpp),
        {
            iv::SourceRange {
                .start = { .line = 7, .column = 1 },
                .end = { .line = 15, .column = 1 },
            },
        }
    );
    ASSERT_FALSE(initial.nodes.empty());

    auto const original_text = iv::test::read_text(module_cpp);
    iv::test::write_text(module_cpp, original_text + "\n");

    std::this_thread::sleep_for(1s);
    auto const reloaded = service.query_by_spans(
        std::filesystem::weakly_canonical(module_cpp),
        {
            iv::SourceRange {
                .start = { .line = 7, .column = 1 },
                .end = { .line = 16, .column = 1 },
            },
        }
    );

    iv::test::write_text(module_cpp, original_text);

    EXPECT_FALSE(reloaded.nodes.empty());
    EXPECT_NO_THROW((void)service.get_logical_node(initial.nodes.front().id));
}

TEST(RuntimeProjectIntrospection, ProjectConfigOverridesIntravenousDefaultsToolchain)
{
    auto const workspace = copy_fixture_workspace("runtime_project_toolchain_override", "local_cmake");
    auto const install_dir = workspace / "install";
    std::filesystem::create_directories(install_dir);
    std::filesystem::remove_all(iv::test::runtime_module_workspace_root("iv.test.local_cmake", workspace));

    write_text(
        install_dir / ".intravenous_defaults",
        "cCompiler=/definitely/missing-clang\n"
        "cxxCompiler=/definitely/missing-clangxx\n"
    );

    auto const c_compiler = configured_program_or_find("clang", IV_CONFIGURED_C_COMPILER);
    auto const cxx_compiler = configured_program_or_find("clang++", IV_CONFIGURED_CXX_COMPILER);
    ASSERT_FALSE(c_compiler.empty());
    ASSERT_FALSE(cxx_compiler.empty());

    write_text(
        workspace / ".intravenous",
        "cCompiler=" + c_compiler + "\n"
        "cxxCompiler=" + cxx_compiler + "\n"
    );

    ScopedEnvVar env("INTRAVENOUS_DIR", install_dir.string());
    BoundRuntimeProjectIntrospection service(workspace, iv::test::repo_root(), {});
    auto const initialized = service.initialize();

    EXPECT_EQ(initialized.module_root, std::filesystem::weakly_canonical(workspace));
}
