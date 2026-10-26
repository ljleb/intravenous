#include "../module_test_utils.h"

#include <intravenous/bridge.h>
#include <intravenous/runtime/package_definitions.h>
#include <intravenous/runtime/package_pipeline_events.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {
struct FakeNodeDefinitions {
    std::size_t calls = 0;
    std::vector<std::shared_ptr<iv::PackageDefinitionsSnapshot const>> snapshots{};
    std::function<void(iv::PackageDefinitionsPublicationRequest&)> respond{};

    void handle_publication(iv::PackageDefinitionsPublicationRequest& request)
    {
        ++calls;
        snapshots.push_back(request.snapshot);
        if (respond) respond(request);
    }
};

using namespace iv;
IV_DECLARE_BRIDGE(
    package_definitions_fake_node_definitions_bridge,
    iv::PackageDefinitions,
    FakeNodeDefinitions);
IV_DEFINE_BRIDGE(package_definitions_fake_node_definitions_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    package_definitions_fake_node_definitions_bridge,
    iv_runtime_package_definitions_publication_requested_event,
    &FakeNodeDefinitions::handle_publication)

struct DefinitionsHarness {
    iv::PackageDefinitions packages;
    FakeNodeDefinitions definitions{};
    package_definitions_fake_node_definitions_bridge::scope scope;

    explicit DefinitionsHarness(std::filesystem::path project_root)
        : packages(std::move(project_root))
        , scope(packages, definitions)
    {}
};

iv::IvPackageDeclaration declaration(
    std::string package_id,
    std::filesystem::path const& package_root)
{
    return iv::IvPackageDeclaration{
        .package_id = std::move(package_id),
        .package_root = std::filesystem::weakly_canonical(package_root),
    };
}

iv::PackageRevision revision(
    iv::IvPackageDeclaration const& declaration,
    std::uint64_t revision_number)
{
    return iv::PackageRevision{
        .package_id = declaration.package_id,
        .package_root = declaration.package_root,
        .revision = revision_number,
    };
}

iv::IvPackageInfo const& only_package(std::vector<iv::IvPackageInfo> const& packages)
{
    EXPECT_EQ(packages.size(), 1u);
    return packages.front();
}
} // namespace

TEST(PackageDefinitions, SuccessfulRevisionBecomesAcceptedAndPublicationProjectionUpdatesCatalog)
{
    auto const project = iv::test_support::fresh_module_fixture_workspace(
        "package_definitions_success");
    auto const root = project / "modules" / "voice";
    std::filesystem::create_directories(root);
    auto const package = declaration("iv.test.voice", root);

    DefinitionsHarness harness(project);
    harness.definitions.respond = [&](iv::PackageDefinitionsPublicationRequest& request) {
        request.published_module_ids_by_package_id[package.package_id] = {
            "iv.test.voice", "iv.test.voice.mod"};
        request.published_leaf_ids_by_package_id[package.package_id] = {"iv.test.osc"};
    };

    auto const initial = harness.packages.snapshot();
    ASSERT_NE(initial, nullptr);
    EXPECT_EQ(initial->generation, 0u);

    harness.packages.handle_package_refresh(iv::PackageRefreshTransaction{
        .declarations = iv::IvPackageDeclarationsChanged{.created = {package}},
        .successful_revisions = {revision(package, 7)},
    });

    ASSERT_EQ(harness.definitions.calls, 1u);
    auto const accepted = harness.packages.snapshot();
    ASSERT_NE(accepted, initial);
    EXPECT_EQ(accepted->generation, 1u);
    ASSERT_TRUE(accepted->by_package_id.contains(package.package_id));
    EXPECT_EQ(accepted->by_package_id.at(package.package_id)->revision, 7u);

    auto const listed = harness.packages.list_packages();
    auto const& info = only_package(listed);
    EXPECT_EQ(info.package_id, package.package_id);
    EXPECT_EQ(info.package_root, package.package_root);
    EXPECT_TRUE(info.project_local);
    EXPECT_EQ(info.build_state, iv::PackageBuildState::built);
    EXPECT_EQ(
        info.module_ids,
        (std::vector<std::string>{"iv.test.voice", "iv.test.voice.mod"}));
    EXPECT_EQ(info.node_type_ids, std::vector<std::string>{"iv.test.osc"});
    EXPECT_TRUE(info.build_message.empty());
    EXPECT_TRUE(info.publication_message.empty());
}

TEST(PackageDefinitions, FailedRebuildPreservesAcceptedSnapshotAndPublishedProjection)
{
    auto const project = iv::test_support::fresh_module_fixture_workspace(
        "package_definitions_failure_preserves");
    auto const root = project / "modules" / "voice";
    std::filesystem::create_directories(root);
    auto const package = declaration("iv.test.voice", root);

    DefinitionsHarness harness(project);
    harness.definitions.respond = [&](iv::PackageDefinitionsPublicationRequest& request) {
        request.published_module_ids_by_package_id[package.package_id] = {"iv.test.voice"};
    };
    harness.packages.handle_package_refresh(iv::PackageRefreshTransaction{
        .declarations = iv::IvPackageDeclarationsChanged{.created = {package}},
        .successful_revisions = {revision(package, 1)},
    });
    auto const accepted_before = harness.packages.snapshot();
    ASSERT_EQ(harness.definitions.calls, 1u);

    harness.packages.handle_package_refresh(iv::PackageRefreshTransaction{
        .failed_builds = {iv::PackageJitFailure{
            .package_id = package.package_id,
            .package_root = package.package_root,
            .message = "compiler exploded",
        }},
    });

    EXPECT_EQ(harness.packages.snapshot(), accepted_before);
    EXPECT_EQ(harness.definitions.calls, 1u);
    auto const listed = harness.packages.list_packages();
    auto const& info = only_package(listed);
    EXPECT_EQ(info.build_state, iv::PackageBuildState::failed);
    EXPECT_EQ(info.build_message, "compiler exploded");
    EXPECT_EQ(info.module_ids, std::vector<std::string>{"iv.test.voice"});
    ASSERT_TRUE(accepted_before->by_package_id.contains(package.package_id));
    EXPECT_EQ(accepted_before->by_package_id.at(package.package_id)->revision, 1u);
}

TEST(PackageDefinitions, RemovingPackagePublishesNewEmptySnapshotAndDeletesCatalogEntry)
{
    auto const project = iv::test_support::fresh_module_fixture_workspace(
        "package_definitions_remove");
    auto const root = project / "modules" / "voice";
    std::filesystem::create_directories(root);
    auto const package = declaration("iv.test.voice", root);

    DefinitionsHarness harness(project);
    harness.packages.handle_package_refresh(iv::PackageRefreshTransaction{
        .declarations = iv::IvPackageDeclarationsChanged{.created = {package}},
        .successful_revisions = {revision(package, 1)},
    });
    auto const first = harness.packages.snapshot();
    ASSERT_EQ(harness.definitions.calls, 1u);

    harness.packages.handle_package_refresh(iv::PackageRefreshTransaction{
        .declarations = iv::IvPackageDeclarationsChanged{
            .deleted_package_ids = {package.package_id},
        },
    });

    ASSERT_EQ(harness.definitions.calls, 2u);
    auto const removed = harness.packages.snapshot();
    EXPECT_NE(removed, first);
    EXPECT_EQ(removed->generation, first->generation + 1);
    EXPECT_TRUE(removed->by_package_id.empty());
    EXPECT_TRUE(harness.packages.list_packages().empty());

    ASSERT_TRUE(first->by_package_id.contains(package.package_id));
    EXPECT_EQ(first->by_package_id.at(package.package_id)->revision, 1u);
}

TEST(PackageDefinitions, PublicationFailureDoesNotRollBackAcceptedPackageRevision)
{
    auto const project = iv::test_support::fresh_module_fixture_workspace(
        "package_definitions_publication_failure");
    auto const root = project / "modules" / "voice";
    std::filesystem::create_directories(root);
    auto const package = declaration("iv.test.voice", root);

    DefinitionsHarness harness(project);
    harness.definitions.respond = [&](iv::PackageDefinitionsPublicationRequest& request) {
        if (request.snapshot->by_package_id.at(package.package_id)->revision == 1) {
            request.published_module_ids_by_package_id[package.package_id] = {"iv.test.voice"};
            return;
        }
        request.published_module_ids_by_package_id[package.package_id] = {"iv.test.voice"};
        request.publication_messages_by_package_id[package.package_id] =
            "synthetic namespace collision";
    };

    harness.packages.handle_package_refresh(iv::PackageRefreshTransaction{
        .declarations = iv::IvPackageDeclarationsChanged{.created = {package}},
        .successful_revisions = {revision(package, 1)},
    });
    auto const first = harness.packages.snapshot();

    harness.packages.handle_package_refresh(iv::PackageRefreshTransaction{
        .successful_revisions = {revision(package, 2)},
    });

    auto const second = harness.packages.snapshot();
    ASSERT_NE(second, first);
    ASSERT_TRUE(second->by_package_id.contains(package.package_id));
    EXPECT_EQ(second->by_package_id.at(package.package_id)->revision, 2u);
    ASSERT_EQ(harness.definitions.calls, 2u);

    auto const listed = harness.packages.list_packages();
    auto const& info = only_package(listed);
    EXPECT_EQ(info.build_state, iv::PackageBuildState::built);
    EXPECT_EQ(info.publication_message, "synthetic namespace collision");
    EXPECT_EQ(info.module_ids, std::vector<std::string>{"iv.test.voice"});

    ASSERT_TRUE(first->by_package_id.contains(package.package_id));
    EXPECT_EQ(first->by_package_id.at(package.package_id)->revision, 1u);
}

TEST(PackageDefinitions, AcceptedSnapshotsRemainImmutableAcrossLaterSuccessfulRevisions)
{
    auto const project = iv::test_support::fresh_module_fixture_workspace(
        "package_definitions_snapshot_immutability");
    auto const root = project / "modules" / "voice";
    std::filesystem::create_directories(root);
    auto const package = declaration("iv.test.voice", root);

    DefinitionsHarness harness(project);
    harness.packages.handle_package_refresh(iv::PackageRefreshTransaction{
        .declarations = iv::IvPackageDeclarationsChanged{.created = {package}},
        .successful_revisions = {revision(package, 41)},
    });
    auto const old_snapshot = harness.packages.snapshot();
    auto const old_revision = old_snapshot->by_package_id.at(package.package_id);

    harness.packages.handle_package_refresh(iv::PackageRefreshTransaction{
        .successful_revisions = {revision(package, 42)},
    });
    auto const new_snapshot = harness.packages.snapshot();

    EXPECT_NE(new_snapshot, old_snapshot);
    EXPECT_EQ(old_snapshot->by_package_id.at(package.package_id), old_revision);
    EXPECT_EQ(old_revision->revision, 41u);
    EXPECT_EQ(new_snapshot->by_package_id.at(package.package_id)->revision, 42u);
}

TEST(PackageDefinitions, ProjectPackageCreationRejectsTraversalAndLeavesNoArtifacts)
{
    auto const project = iv::test_support::fresh_module_fixture_workspace(
        "package_definitions_create_rejects_bad_names");
    DefinitionsHarness harness(project);

    for (std::string const bad_name : {"", ".", "..", "../escape", "slash/name", "9starts_digit", "has space"}) {
        EXPECT_THROW(
            static_cast<void>(harness.packages.create_project_package(bad_name)),
            std::runtime_error);
    }
    EXPECT_FALSE(std::filesystem::exists(project / "modules" / "escape"));
}

TEST(PackageDefinitions, ProjectPackageCreationWritesSelfConsistentSkeletonAndRejectsOverwrite)
{
    auto const project = iv::test_support::fresh_module_fixture_workspace(
        "package_definitions_create_valid");
    DefinitionsHarness harness(project);

    auto const created = harness.packages.create_project_package("_voice-bank");
    auto const expected_root = std::filesystem::weakly_canonical(
        project / "modules" / "_voice-bank");
    EXPECT_EQ(created.package_root, expected_root);
    EXPECT_EQ(created.package_id, expected_root.generic_string());
    EXPECT_TRUE(created.project_local);

    auto const manifest = iv::test_support::read_text(expected_root / "iv_package.json");
    EXPECT_NE(manifest.find(R"("schema": 2)"), std::string::npos);
    EXPECT_NE(manifest.find(R"("entry": "module.cpp")"), std::string::npos);
    auto const source = iv::test_support::read_text(expected_root / "module.cpp");
    EXPECT_NE(source.find("iv.project._voice_bank"), std::string::npos);
    EXPECT_TRUE(std::filesystem::is_regular_file(expected_root / "compile_commands.json"));

    auto const source_before = source;
    EXPECT_THROW(
        static_cast<void>(harness.packages.create_project_package("_voice-bank")),
        std::runtime_error);
    EXPECT_EQ(iv::test_support::read_text(expected_root / "module.cpp"), source_before);
}
