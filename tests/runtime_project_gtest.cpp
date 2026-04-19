#include "module_test_utils.h"
#include "fake_audio_device.h"

#include "runtime/project_service.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <set>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {
    using namespace std::chrono_literals;

    iv::Timeline& runtime_timeline()
    {
        static thread_local iv::Timeline timeline;
        return timeline;
    }

    struct DrivenTestAudioDevice {
        std::shared_ptr<iv::test::FakeAudioDevice> device;
        std::jthread driver;

        auto make_factory() const
        {
            return [device = this->device]() -> std::optional<iv::LogicalAudioDevice> {
                struct RefBackend {
                    std::shared_ptr<iv::test::FakeAudioDevice> device;

                    iv::RenderConfig const& config() const
                    {
                        return device->config();
                    }

                    std::span<iv::Sample> wait_for_block_request()
                    {
                        return device->wait_for_block_request();
                    }

                    void submit_response()
                    {
                        try {
                            device->submit_response();
                        } catch (std::logic_error const&) {
                        }
                    }
                };

                return iv::LogicalAudioDevice(RefBackend { device });
            };
        }
    };

    DrivenTestAudioDevice make_audio_device_context()
    {
        auto device = std::make_shared<iv::test::FakeAudioDevice>(iv::RenderConfig {
            .sample_rate = 48000,
            .num_channels = 2,
            .max_block_frames = 256,
            .preferred_block_size = 64,
        });

        std::jthread driver([device](std::stop_token stop_token) {
            size_t frame_index = 0;
            while (!stop_token.stop_requested()) {
                device->begin_requested_block(frame_index, device->config().preferred_block_size);
                if (!device->wait_until_block_ready_for(200ms)) {
                    break;
                }
                device->finish_requested_block();
                frame_index += device->config().preferred_block_size;
                std::this_thread::sleep_for(1ms);
            }
            device->request_shutdown();
        });

        return DrivenTestAudioDevice {
            .device = std::move(device),
            .driver = std::move(driver),
        };
    }

    std::filesystem::path make_workspace(std::string const& name)
    {
        auto const workspace = iv::test::runtime_modules_root() / name;
        std::filesystem::remove_all(workspace);
        std::filesystem::create_directories(workspace);
        return workspace;
    }

    void write_text(std::filesystem::path const& path, std::string const& text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(static_cast<bool>(out)) << "failed to open " << path;
        out << text;
    }

    std::filesystem::path copy_fixture_workspace(std::string const& test_name, std::string const& fixture_name)
    {
        auto const workspace = make_workspace(test_name);
        iv::test::copy_directory(iv::test::test_modules_root() / fixture_name, workspace);
        return workspace;
    }

    std::filesystem::path make_inline_module_workspace(std::string const& test_name, std::string const& module_text)
    {
        auto const workspace = make_workspace(test_name);
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
}

TEST(RuntimeProjectService, EmptyIntravenousMarkerUsesWorkspaceRoot)
{
    auto const workspace = copy_fixture_workspace("runtime_project_empty_marker", "local_cmake");
    write_text(workspace / ".intravenous", "");

    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
    auto const initialized = service.initialize();

    EXPECT_EQ(initialized.module_root, std::filesystem::weakly_canonical(workspace));
    EXPECT_EQ(initialized.execution_epoch, 1u);
    EXPECT_FALSE(initialized.module_id.empty());
}

TEST(RuntimeProjectService, RelativeRootModulePathResolvesAgainstWorkspaceRoot)
{
    auto const workspace = make_workspace("runtime_project_relative_root");
    auto const module_root = workspace / "module";
    iv::test::copy_directory(iv::test::test_modules_root() / "local_cmake", module_root);
    write_text(workspace / ".intravenous", "rootModulePath=module\n");

    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
    auto const initialized = service.initialize();

    EXPECT_EQ(initialized.module_root, std::filesystem::weakly_canonical(module_root));
    EXPECT_EQ(initialized.execution_epoch, 1u);
}

TEST(RuntimeProjectService, QueryBySpansReturnsMatchingLiveNodesWithPorts)
{
    auto const workspace = copy_fixture_workspace("runtime_project_query_by_spans", "local_cmake");
    write_text(workspace / ".intravenous", "");

    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
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

    ASSERT_EQ(result.execution_epoch, initialized.execution_epoch);
    ASSERT_FALSE(result.nodes.empty());

    auto const& node = result.nodes.front();
    EXPECT_FALSE(node.id.empty());
    EXPECT_FALSE(node.kind.empty());
    EXPECT_FALSE(node.source_spans.empty());

    bool has_any_port = !node.sample_inputs.empty() || !node.sample_outputs.empty() || !node.event_inputs.empty() || !node.event_outputs.empty();
    EXPECT_TRUE(has_any_port);
}

TEST(RuntimeProjectService, QueryBySpansKeepsDistinctDeclarationsSeparate)
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

    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
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

TEST(RuntimeProjectService, QueryBySpansKeepsAnnotatedLogicalNodeIdStableAcrossReload)
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

    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
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

    iv::RuntimeProjectQueryResult reloaded;
    auto const deadline = std::chrono::steady_clock::now() + 45s;
    do {
        std::this_thread::sleep_for(100ms);
        reloaded = service.query_by_spans(
            module_cpp,
            {
                iv::SourceRange {
                    .start = { .line = 1, .column = 1 },
                    .end = { .line = 25, .column = 1 },
                },
            }
        );
        if (reloaded.execution_epoch > initial.execution_epoch) {
            break;
        }
    } while (std::chrono::steady_clock::now() < deadline);

    iv::test::write_text(module_cpp, original_text);

    ASSERT_GT(reloaded.execution_epoch, initial.execution_epoch);
    auto const reloaded_it = std::find_if(reloaded.nodes.begin(), reloaded.nodes.end(), [](auto const& node) {
        return node.kind.contains("ValueSource");
    });

    ASSERT_NE(reloaded_it, reloaded.nodes.end());
    EXPECT_EQ(reloaded_it->id, initial_id);
}

TEST(RuntimeProjectService, QueryBySpansReturnsAnnotatedLogicalNode)
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

    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
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

TEST(RuntimeProjectService, QueryBySpansReturnsSingleAssignedDeclarationBackedRef)
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
        SamplePortRef x;
        x = g.node<ValueSource>(&context.sample_period()).node_ref();
        auto const sink = context.target_factory().sink(g, 0);
        sink(x);
        g.outputs();
    }
}

IV_EXPORT_MODULE("iv.test.assigned_ref_module", assigned_ref_module);
)"
    );

    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
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

TEST(RuntimeProjectService, InitializationFailsWhenDeclarationBackedRefIsAssignedTwice)
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
        SamplePortRef x;
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

    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
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

TEST(RuntimeProjectService, QueryBySpansDoesNotMergeDifferentSchemas)
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
        return g.node<iv::Sum>(Inputs).node_ref();
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

    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
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

TEST(RuntimeProjectService, QueryBySpansAggregatesMixedConnectivity)
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
        return g.node<iv::Sum>(1).node_ref();
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

    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
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

TEST(RuntimeProjectService, QueryBySpansIntersectsMultipleSelections)
{
    auto const workspace = copy_fixture_workspace("runtime_project_query_by_spans_intersection", "local_cmake");
    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    write_text(workspace / ".intravenous", "");

    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
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

    EXPECT_EQ(both.execution_epoch, dt_only.execution_epoch);
    EXPECT_EQ(both.execution_epoch, sink_only.execution_epoch);
    EXPECT_EQ(both_ids, intersection);
}

TEST(RuntimeProjectService, QueryBySpansUnionsMultipleSelections)
{
    auto const workspace = copy_fixture_workspace("runtime_project_query_by_spans_union", "local_cmake");
    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    write_text(workspace / ".intravenous", "");

    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
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

    EXPECT_EQ(both.execution_epoch, dt_only.execution_epoch);
    EXPECT_EQ(both.execution_epoch, sink_only.execution_epoch);
    EXPECT_EQ(both_ids, expected_union);
}

TEST(RuntimeProjectService, QueryActiveRegionsReturnsOnlySourceSpans)
{
    auto const workspace = copy_fixture_workspace("runtime_project_query_active_regions", "local_cmake");
    auto const module_cpp = std::filesystem::weakly_canonical(workspace / "module.cpp");
    write_text(workspace / ".intravenous", "");

    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
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

    EXPECT_EQ(active_regions.execution_epoch, nodes.execution_epoch);
    EXPECT_EQ(actual_spans, expected_spans);
}

TEST(RuntimeProjectService, QueryBySpansMergesPolyphonicCallbackNodesByExactSourceSpan)
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
    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
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
    ASSERT_EQ(logical.sample_inputs.size(), 3u);
    EXPECT_EQ(logical.sample_inputs[0].name, "phase_offset");
    EXPECT_EQ(logical.sample_inputs[1].name, "frequency");
    EXPECT_EQ(logical.sample_inputs[2].name, "dt");
    ASSERT_EQ(logical.sample_outputs.size(), 1u);
    EXPECT_EQ(logical.sample_outputs[0].name, "out");

    auto const resolved = service.get_logical_node(result.execution_epoch, logical.id);
    EXPECT_EQ(resolved.kind, "iv::SawOscillator");
    EXPECT_EQ(resolved.member_count, 2u);
    ASSERT_EQ(resolved.sample_inputs.size(), 3u);
    EXPECT_EQ(resolved.sample_inputs[0].name, "phase_offset");
    EXPECT_EQ(resolved.sample_inputs[1].name, "frequency");
    EXPECT_EQ(resolved.sample_inputs[2].name, "dt");
    ASSERT_EQ(resolved.sample_outputs.size(), 1u);
    EXPECT_EQ(resolved.sample_outputs[0].name, "out");

    EXPECT_TRUE(std::ranges::none_of(result.nodes, [](auto const& node) {
        return node.kind == "Polyphonic";
    }));
}

TEST(RuntimeProjectService, QueryBySpansDoesNotAttributeInteriorPolyphonicLambdaSpansToOuterSubgraph)
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
    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
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

TEST(RuntimeProjectService, MissingMarkerFailsInitialization)
{
    auto const workspace = copy_fixture_workspace("runtime_project_missing_marker", "local_cmake");

    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
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

TEST(RuntimeProjectService, ReloadInvalidatesOldNodeIds)
{
    auto const workspace = copy_fixture_workspace("runtime_project_reload_epoch", "local_cmake");
    auto const module_cpp = workspace / "module.cpp";
    write_text(workspace / ".intravenous", "");

    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
    auto const initialized = service.initialize();

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

    iv::RuntimeProjectQueryResult reloaded;
    auto const deadline = std::chrono::steady_clock::now() + 45s;
    do {
        std::this_thread::sleep_for(100ms);
        reloaded = service.query_by_spans(
            std::filesystem::weakly_canonical(module_cpp),
            {
                iv::SourceRange {
                    .start = { .line = 7, .column = 1 },
                    .end = { .line = 16, .column = 1 },
                },
            }
        );
        if (reloaded.execution_epoch > initialized.execution_epoch) {
            break;
        }
    } while (std::chrono::steady_clock::now() < deadline);

    iv::test::write_text(module_cpp, original_text);

    ASSERT_GT(reloaded.execution_epoch, initialized.execution_epoch);
    EXPECT_THROW(
        {
            try {
                (void)service.get_logical_node(initialized.execution_epoch, initial.nodes.front().id);
            } catch (std::exception const& e) {
                EXPECT_TRUE(std::string(e.what()).contains("stale"));
                throw;
            }
        },
        std::exception
    );
}

TEST(RuntimeProjectService, ProjectConfigOverridesIntraveniousDefaultsToolchain)
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
    auto audio = make_audio_device_context();
    iv::RuntimeProjectService service(runtime_timeline(), workspace, iv::test::repo_root(), {}, audio.make_factory());
    auto const initialized = service.initialize();

    EXPECT_EQ(initialized.module_root, std::filesystem::weakly_canonical(workspace));
    EXPECT_EQ(initialized.execution_epoch, 1u);
}
