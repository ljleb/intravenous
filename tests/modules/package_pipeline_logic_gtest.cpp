#include "../module_test_utils.h"

#include <intravenous/bridge.h>
#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/package_definitions.h>
#include <intravenous/runtime/package_definitions_node_definitions_bridge.h>
#include <intravenous/runtime/package_pipeline_events.h>
#include <intravenous/runtime/package_watcher.h>
#include <intravenous/runtime/package_watcher_package_definitions_bridge.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace {
struct FakePipelineJit {
    std::size_t calls = 0;
    std::function<void(iv::PackageJitBatchRequest&)> respond{};

    void handle_build_request(iv::PackageJitBatchRequest& request)
    {
        ++calls;
        if (respond) respond(request);
    }
};

using namespace iv;
IV_DECLARE_BRIDGE(
    package_pipeline_logic_fake_jit_bridge,
    iv::PackageWatcher,
    FakePipelineJit);
IV_DEFINE_BRIDGE(package_pipeline_logic_fake_jit_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    package_pipeline_logic_fake_jit_bridge,
    iv_runtime_package_jit_batch_requested_event,
    &FakePipelineJit::handle_build_request)

struct PipelineLogicHarness {
    iv::PackageWatcher watcher{};
    FakePipelineJit jit{};
    iv::PackageDefinitions packages;
    iv::NodeDefinitions definitions{};
    package_pipeline_logic_fake_jit_bridge::scope watcher_jit_scope;
    iv::package_watcher_package_definitions_bridge::scope watcher_packages_scope;
    iv::package_definitions_node_definitions_bridge::scope packages_definitions_scope;

    explicit PipelineLogicHarness(std::filesystem::path project_root)
        : packages(std::move(project_root))
        , watcher_jit_scope(watcher, jit)
        , watcher_packages_scope(watcher, packages)
        , packages_definitions_scope(packages, definitions)
    {}

    bool refresh()
    {
        iv::PackageWatcherRefreshRequest request;
        watcher.handle_refresh_requested(request);
        return request.changed;
    }
};

std::pair<std::string, std::filesystem::path> declaration(
    std::string package_id,
    std::filesystem::path package_root)
{
    return {std::move(package_id), std::filesystem::weakly_canonical(package_root)};
}

iv::PackageModuleDefinition module_definition(
    iv::IvPackageDeclaration const& declaration,
    std::string definition_id)
{
    return iv::PackageModuleDefinition{
        .package_id = declaration.package_id,
        .definition_id = definition_id,
        .package_root = declaration.package_root,
        .module_id = std::move(definition_id),
    };
}

iv::PackageRevision revision_with_module(
    iv::IvPackageDeclaration const& declaration,
    std::string definition_id,
    std::uint64_t revision = 1)
{
    return iv::PackageRevision{
        .package_id = declaration.package_id,
        .package_root = declaration.package_root,
        .revision = revision,
        .module_definitions = {
            module_definition(declaration, std::move(definition_id)),
        },
    };
}

iv::IvPackageInfo const* find_package(
    std::vector<iv::IvPackageInfo> const& packages,
    std::string const& package_id)
{
    auto it = std::ranges::find(packages, package_id, &iv::IvPackageInfo::package_id);
    return it == packages.end() ? nullptr : &*it;
}
} // namespace

TEST(PackagePipelineLogic, MixedSuccessAndFailurePublishesOnlyAcceptedRevisionAtomically)
{
    auto const project = iv::test_support::fresh_module_fixture_workspace(
        "package_pipeline_logic_mixed");
    auto const good_root = project / "good";
    auto const bad_root = project / "bad";
    std::filesystem::create_directories(good_root);
    std::filesystem::create_directories(bad_root);
    auto const good = iv::IvPackageDeclaration{
        .package_id = "iv.test.good",
        .package_root = std::filesystem::weakly_canonical(good_root),
    };
    auto const bad = iv::IvPackageDeclaration{
        .package_id = "iv.test.bad",
        .package_root = std::filesystem::weakly_canonical(bad_root),
    };

    PipelineLogicHarness pipeline(project);
    pipeline.jit.respond = [&](iv::PackageJitBatchRequest& request) {
        ASSERT_EQ(request.declarations.size(), 2u);
        request.result.revisions.push_back(
            revision_with_module(good, "iv.test.good.module"));
        request.result.failed.push_back(iv::PackageJitFailure{
            .package_id = bad.package_id,
            .package_root = bad.package_root,
            .message = "synthetic compile failure",
        });
    };

    pipeline.watcher.synchronize_discovered_packages({
        declaration(good.package_id, good.package_root),
        declaration(bad.package_id, bad.package_root),
    });
    ASSERT_TRUE(pipeline.refresh());

    ASSERT_EQ(pipeline.jit.calls, 1u);
    auto const package_snapshot = pipeline.packages.snapshot();
    ASSERT_EQ(package_snapshot->by_package_id.size(), 1u);
    EXPECT_TRUE(package_snapshot->by_package_id.contains(good.package_id));
    EXPECT_FALSE(package_snapshot->by_package_id.contains(bad.package_id));

    auto const definitions = pipeline.definitions.loaded_module_definitions();
    ASSERT_EQ(definitions.size(), 1u);
    EXPECT_EQ(definitions.front().definition_id, "iv.test.good.module");

    auto const packages = pipeline.packages.list_packages();
    auto const* good_info = find_package(packages, good.package_id);
    auto const* bad_info = find_package(packages, bad.package_id);
    ASSERT_NE(good_info, nullptr);
    ASSERT_NE(bad_info, nullptr);
    EXPECT_EQ(good_info->build_state, iv::PackageBuildState::built);
    EXPECT_EQ(bad_info->build_state, iv::PackageBuildState::failed);
    EXPECT_EQ(bad_info->build_message, "synthetic compile failure");
    EXPECT_FALSE(pipeline.refresh());
}

TEST(PackagePipelineLogic, NamespaceCollisionAcceptsPackagesButPreservesPreviousGlobalNamespace)
{
    auto const project = iv::test_support::fresh_module_fixture_workspace(
        "package_pipeline_logic_collision");
    auto const first_root = project / "first";
    auto const second_root = project / "second";
    std::filesystem::create_directories(first_root);
    std::filesystem::create_directories(second_root);
    auto const first = iv::IvPackageDeclaration{
        .package_id = "iv.test.first",
        .package_root = std::filesystem::weakly_canonical(first_root),
    };
    auto const second = iv::IvPackageDeclaration{
        .package_id = "iv.test.second",
        .package_root = std::filesystem::weakly_canonical(second_root),
    };

    PipelineLogicHarness pipeline(project);
    pipeline.jit.respond = [&](iv::PackageJitBatchRequest& request) {
        for (auto const& built : request.declarations) {
            if (built.package_id == first.package_id) {
                request.result.revisions.push_back(
                    revision_with_module(first, "iv.test.shared"));
            } else if (built.package_id == second.package_id) {
                request.result.revisions.push_back(
                    revision_with_module(second, "iv.test.shared"));
            }
        }
    };

    pipeline.watcher.synchronize_discovered_packages({
        declaration(first.package_id, first.package_root),
    });
    ASSERT_TRUE(pipeline.refresh());
    auto const namespace_before_collision = pipeline.definitions.snapshot();
    ASSERT_TRUE(namespace_before_collision->by_id.contains("iv.test.shared"));
    EXPECT_EQ(
        std::get<iv::ModuleNodeDefinition>(
            namespace_before_collision->by_id.at("iv.test.shared").definition).package_id,
        first.package_id);

    pipeline.watcher.synchronize_discovered_packages({
        declaration(first.package_id, first.package_root),
        declaration(second.package_id, second.package_root),
    });
    ASSERT_TRUE(pipeline.refresh());

    auto const accepted = pipeline.packages.snapshot();
    ASSERT_EQ(accepted->by_package_id.size(), 2u);
    EXPECT_TRUE(accepted->by_package_id.contains(first.package_id));
    EXPECT_TRUE(accepted->by_package_id.contains(second.package_id));
    EXPECT_EQ(pipeline.definitions.snapshot(), namespace_before_collision);

    auto const packages = pipeline.packages.list_packages();
    auto const* first_info = find_package(packages, first.package_id);
    auto const* second_info = find_package(packages, second.package_id);
    ASSERT_NE(first_info, nullptr);
    ASSERT_NE(second_info, nullptr);
    EXPECT_FALSE(first_info->publication_message.empty());
    EXPECT_FALSE(second_info->publication_message.empty());
    EXPECT_NE(first_info->publication_message.find("multiple IV packages"), std::string::npos);
    EXPECT_EQ(first_info->publication_message, second_info->publication_message);
}

TEST(PackagePipelineLogic, RemovingCollidingPackageClearsDiagnosticsWithoutRepublishingUnchangedProvider)
{
    auto const project = iv::test_support::fresh_module_fixture_workspace(
        "package_pipeline_logic_collision_recovery");
    auto const first_root = project / "first";
    auto const second_root = project / "second";
    std::filesystem::create_directories(first_root);
    std::filesystem::create_directories(second_root);
    auto const first = iv::IvPackageDeclaration{
        .package_id = "iv.test.first",
        .package_root = std::filesystem::weakly_canonical(first_root),
    };
    auto const second = iv::IvPackageDeclaration{
        .package_id = "iv.test.second",
        .package_root = std::filesystem::weakly_canonical(second_root),
    };

    PipelineLogicHarness pipeline(project);
    pipeline.jit.respond = [&](iv::PackageJitBatchRequest& request) {
        for (auto const& built : request.declarations) {
            auto const& source = built.package_id == first.package_id ? first : second;
            request.result.revisions.push_back(
                revision_with_module(source, "iv.test.shared"));
        }
    };

    pipeline.watcher.synchronize_discovered_packages({
        declaration(first.package_id, first.package_root),
    });
    ASSERT_TRUE(pipeline.refresh());
    auto const namespace_after_first = pipeline.definitions.snapshot();

    pipeline.watcher.synchronize_discovered_packages({
        declaration(first.package_id, first.package_root),
        declaration(second.package_id, second.package_root),
    });
    ASSERT_TRUE(pipeline.refresh());
    ASSERT_EQ(pipeline.definitions.snapshot(), namespace_after_first);
    ASSERT_FALSE(find_package(pipeline.packages.list_packages(), first.package_id)
        ->publication_message.empty());

    pipeline.watcher.synchronize_discovered_packages({
        declaration(first.package_id, first.package_root),
    });
    ASSERT_TRUE(pipeline.refresh());

    EXPECT_EQ(pipeline.definitions.snapshot(), namespace_after_first);
    auto const packages = pipeline.packages.list_packages();
    ASSERT_EQ(packages.size(), 1u);
    EXPECT_EQ(packages.front().package_id, first.package_id);
    EXPECT_TRUE(packages.front().publication_message.empty());
    EXPECT_EQ(packages.front().module_ids, std::vector<std::string>{"iv.test.shared"});
}
