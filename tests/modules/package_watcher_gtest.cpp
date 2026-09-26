#include "../module_test_utils.h"

#include <intravenous/bridge.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/package_pipeline_events.h>
#include <intravenous/runtime/package_watcher.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace {
struct FakePackageJit {
    struct Call {
        std::vector<iv::IvPackageDeclaration> declarations{};
        std::vector<std::filesystem::path> removed_package_roots{};
    };

    std::vector<Call> calls{};
    std::function<void(iv::PackageJitBatchRequest&)> respond{};

    void handle_build_request(iv::PackageJitBatchRequest& request)
    {
        calls.push_back(Call{
            .declarations = request.declarations,
            .removed_package_roots = request.removed_package_roots,
        });
        if (respond) respond(request);
    }
};

struct FakePackageDefinitions {
    std::vector<iv::PackageRefreshTransaction> transactions{};

    void handle_package_refresh(iv::PackageRefreshTransaction const& transaction)
    {
        transactions.push_back(transaction);
    }
};

using namespace iv;
IV_DECLARE_BRIDGE(
    package_watcher_fake_jit_bridge,
    iv::PackageWatcher,
    FakePackageJit);
IV_DEFINE_BRIDGE(package_watcher_fake_jit_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    package_watcher_fake_jit_bridge,
    iv_runtime_package_jit_batch_requested_event,
    &FakePackageJit::handle_build_request)

IV_DECLARE_BRIDGE(
    package_watcher_fake_definitions_bridge,
    iv::PackageWatcher,
    FakePackageDefinitions);
IV_DEFINE_BRIDGE(package_watcher_fake_definitions_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    package_watcher_fake_definitions_bridge,
    iv_runtime_package_refresh_event,
    &FakePackageDefinitions::handle_package_refresh)

struct WatcherHarness {
    iv::PackageWatcher watcher{};
    FakePackageJit jit{};
    FakePackageDefinitions definitions{};
    package_watcher_fake_jit_bridge::scope jit_scope;
    package_watcher_fake_definitions_bridge::scope definitions_scope;

    WatcherHarness()
        : jit_scope(watcher, jit)
        , definitions_scope(watcher, definitions)
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

iv::PackageRevision revision_for(
    iv::IvPackageDeclaration const& declaration,
    std::uint64_t revision = 1,
    std::vector<iv::ModuleDependency> dependencies = {})
{
    return iv::PackageRevision{
        .package_id = declaration.package_id,
        .package_root = declaration.package_root,
        .revision = revision,
        .dependencies = std::move(dependencies),
    };
}

std::vector<std::string> package_ids(
    std::vector<iv::IvPackageDeclaration> const& declarations)
{
    std::vector<std::string> result;
    result.reserve(declarations.size());
    for (auto const& declaration : declarations) result.push_back(declaration.package_id);
    std::ranges::sort(result);
    return result;
}

std::vector<std::string> revision_ids(std::vector<iv::PackageRevision> const& revisions)
{
    std::vector<std::string> result;
    result.reserve(revisions.size());
    for (auto const& revision : revisions) result.push_back(revision.package_id);
    std::ranges::sort(result);
    return result;
}
} // namespace

TEST(PackageWatcher, CoalescesMultipleDirtyPackagesIntoOneJitCallAndOneTransaction)
{
    auto const first = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_batch_first");
    auto const second = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_batch_second");

    WatcherHarness harness;
    harness.jit.respond = [](iv::PackageJitBatchRequest& request) {
        for (auto const& declaration : request.declarations) {
            request.result.revisions.push_back(revision_for(declaration));
        }
    };

    harness.watcher.synchronize_discovered_packages({
        declaration("iv.test.batch.first", first),
        declaration("iv.test.batch.second", second),
    });

    ASSERT_TRUE(harness.refresh());
    ASSERT_EQ(harness.jit.calls.size(), 1u);
    EXPECT_EQ(
        package_ids(harness.jit.calls.front().declarations),
        (std::vector<std::string>{"iv.test.batch.first", "iv.test.batch.second"}));

    ASSERT_EQ(harness.definitions.transactions.size(), 1u);
    auto const& transaction = harness.definitions.transactions.front();
    EXPECT_EQ(
        package_ids(transaction.declarations.created),
        (std::vector<std::string>{"iv.test.batch.first", "iv.test.batch.second"}));
    EXPECT_TRUE(transaction.declarations.updated.empty());
    EXPECT_TRUE(transaction.declarations.deleted_package_ids.empty());
    EXPECT_EQ(
        revision_ids(transaction.successful_revisions),
        (std::vector<std::string>{"iv.test.batch.first", "iv.test.batch.second"}));
    EXPECT_TRUE(transaction.failed_builds.empty());

    EXPECT_FALSE(harness.watcher.has_pending_refresh());
    EXPECT_FALSE(harness.refresh());
    EXPECT_EQ(harness.jit.calls.size(), 1u);
    EXPECT_EQ(harness.definitions.transactions.size(), 1u);
}

TEST(PackageWatcher, FailedBuildIsForwardedOnceAndDoesNotBusyRetry)
{
    auto const root = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_failed_once");

    WatcherHarness harness;
    harness.jit.respond = [](iv::PackageJitBatchRequest& request) {
        ASSERT_EQ(request.declarations.size(), 1u);
        auto const& declaration = request.declarations.front();
        request.result.failed.push_back(iv::PackageJitFailure{
            .package_id = declaration.package_id,
            .package_root = declaration.package_root,
            .message = "synthetic failure",
        });
    };

    harness.watcher.synchronize_discovered_packages({
        declaration("iv.test.failed.once", root),
    });

    ASSERT_TRUE(harness.refresh());
    ASSERT_EQ(harness.jit.calls.size(), 1u);
    ASSERT_EQ(harness.definitions.transactions.size(), 1u);
    ASSERT_EQ(harness.definitions.transactions.front().failed_builds.size(), 1u);
    EXPECT_EQ(
        harness.definitions.transactions.front().failed_builds.front().message,
        "synthetic failure");

    EXPECT_FALSE(harness.watcher.has_pending_refresh());
    EXPECT_FALSE(harness.refresh());
    EXPECT_EQ(harness.jit.calls.size(), 1u);
    EXPECT_EQ(harness.definitions.transactions.size(), 1u);
}

TEST(PackageWatcher, MovingPackageRootEvictsOldRootAndPublishesOneUpdatedDeclaration)
{
    auto const first_root = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_move_first");
    auto const second_root = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_move_second");
    std::string const package_id = "iv.test.move";

    WatcherHarness harness;
    harness.jit.respond = [](iv::PackageJitBatchRequest& request) {
        for (auto const& declaration : request.declarations) {
            request.result.revisions.push_back(revision_for(declaration));
        }
    };

    harness.watcher.synchronize_discovered_packages({declaration(package_id, first_root)});
    ASSERT_TRUE(harness.refresh());
    harness.jit.calls.clear();
    harness.definitions.transactions.clear();

    harness.watcher.synchronize_discovered_packages({declaration(package_id, second_root)});
    ASSERT_TRUE(harness.refresh());

    ASSERT_EQ(harness.jit.calls.size(), 1u);
    auto const& call = harness.jit.calls.front();
    ASSERT_EQ(call.declarations.size(), 1u);
    EXPECT_EQ(call.declarations.front().package_id, package_id);
    EXPECT_EQ(call.declarations.front().package_root, std::filesystem::weakly_canonical(second_root));
    ASSERT_EQ(call.removed_package_roots.size(), 1u);
    EXPECT_EQ(call.removed_package_roots.front(), std::filesystem::weakly_canonical(first_root));

    ASSERT_EQ(harness.definitions.transactions.size(), 1u);
    auto const& transaction = harness.definitions.transactions.front();
    EXPECT_TRUE(transaction.declarations.created.empty());
    ASSERT_EQ(transaction.declarations.updated.size(), 1u);
    EXPECT_EQ(transaction.declarations.updated.front().package_id, package_id);
    EXPECT_TRUE(transaction.declarations.deleted_package_ids.empty());
}

TEST(PackageWatcher, RemovalEvictsJitRootAndPublishesExactlyOneDeletionTransaction)
{
    auto const root = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_remove");
    std::string const package_id = "iv.test.remove";

    WatcherHarness harness;
    harness.jit.respond = [](iv::PackageJitBatchRequest& request) {
        for (auto const& declaration : request.declarations) {
            request.result.revisions.push_back(revision_for(declaration));
        }
    };

    harness.watcher.synchronize_discovered_packages({declaration(package_id, root)});
    ASSERT_TRUE(harness.refresh());
    harness.jit.calls.clear();
    harness.definitions.transactions.clear();

    harness.watcher.synchronize_discovered_packages({});
    ASSERT_TRUE(harness.refresh());

    ASSERT_EQ(harness.jit.calls.size(), 1u);
    EXPECT_TRUE(harness.jit.calls.front().declarations.empty());
    ASSERT_EQ(harness.jit.calls.front().removed_package_roots.size(), 1u);
    EXPECT_EQ(
        harness.jit.calls.front().removed_package_roots.front(),
        std::filesystem::weakly_canonical(root));

    ASSERT_EQ(harness.definitions.transactions.size(), 1u);
    auto const& transaction = harness.definitions.transactions.front();
    EXPECT_TRUE(transaction.declarations.created.empty());
    EXPECT_TRUE(transaction.declarations.updated.empty());
    EXPECT_EQ(transaction.declarations.deleted_package_ids, std::vector<std::string>{package_id});
    EXPECT_TRUE(transaction.successful_revisions.empty());
    EXPECT_TRUE(transaction.failed_builds.empty());
    EXPECT_FALSE(harness.refresh());
}

TEST(PackageWatcher, DuplicateDiscoverySnapshotThrowsWithoutReplacingPreviousState)
{
    auto const first_root = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_duplicate_first");
    auto const second_root = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_duplicate_second");

    WatcherHarness harness;
    harness.jit.respond = [](iv::PackageJitBatchRequest& request) {
        for (auto const& item : request.declarations) {
            request.result.revisions.push_back(revision_for(item));
        }
    };
    harness.watcher.synchronize_discovered_packages({
        declaration("iv.test.stable", first_root),
    });

    EXPECT_THROW(
        harness.watcher.synchronize_discovered_packages({
            declaration("iv.test.duplicate", first_root),
            declaration("iv.test.duplicate", second_root),
        }),
        std::runtime_error);

    ASSERT_TRUE(harness.refresh());
    ASSERT_EQ(harness.jit.calls.size(), 1u);
    ASSERT_EQ(harness.jit.calls.front().declarations.size(), 1u);
    EXPECT_EQ(harness.jit.calls.front().declarations.front().package_id, "iv.test.stable");
}

TEST(PackageWatcher, ConflictingDeclarationSourcesThrowWithoutPoisoningFutureMerges)
{
    auto const retained_root = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_retained_root");
    auto const conflicting_root = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_conflicting_root");
    auto const retained_id = std::filesystem::weakly_canonical(retained_root).generic_string();

    WatcherHarness harness;
    harness.watcher.handle_required_definitions_changed(iv::IvModuleRequiredDefinitionsChanged{
        .created = {iv::IvModuleRequiredDefinition{
            .definition_id = "iv.test.retained",
            .package_root = retained_root,
        }},
    });

    EXPECT_THROW(
        harness.watcher.synchronize_discovered_packages({
            declaration(retained_id, conflicting_root),
        }),
        std::runtime_error);

    EXPECT_NO_THROW(harness.watcher.handle_required_definitions_changed(
        iv::IvModuleRequiredDefinitionsChanged{
            .updated = {iv::IvModuleRequiredDefinition{
                .definition_id = "iv.test.retained",
                .package_root = retained_root,
            }},
        }));
}

TEST(PackageWatcher, DiscoveryIgnoresMalformedPackagesAndDeduplicatesOverlappingRoots)
{
    auto const root = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_discovery_adversarial");
    auto const good = root / "good";
    auto const malformed = root / "malformed";
    auto const missing_entry = root / "missing_entry";
    auto const absolute_entry = root / "absolute_entry";
    auto const ignored_build = root / "build" / "ignored";
    std::filesystem::create_directories(good);
    std::filesystem::create_directories(malformed);
    std::filesystem::create_directories(missing_entry);
    std::filesystem::create_directories(absolute_entry);
    std::filesystem::create_directories(ignored_build);

    iv::test_support::write_text(good / "iv_package.json", R"({"schema":2,"entry":"module.cpp"})");
    iv::test_support::write_text(good / "module.cpp", "// valid\n");
    iv::test_support::write_text(malformed / "iv_package.json", "{not json");
    iv::test_support::write_text(malformed / "module.cpp", "// ignored\n");
    iv::test_support::write_text(
        missing_entry / "iv_package.json",
        R"({"schema":2,"entry":"missing.cpp"})");
    iv::test_support::write_text(
        absolute_entry / "iv_package.json",
        R"({"schema":2,"entry":"/tmp/not-allowed.cpp"})");
    iv::test_support::write_text(
        ignored_build / "iv_package.json",
        R"({"schema":2,"entry":"module.cpp"})");
    iv::test_support::write_text(ignored_build / "module.cpp", "// ignored\n");

    auto const discovered = iv::discover_iv_package_declarations(root, {root, good});
    ASSERT_EQ(discovered.size(), 1u);
    auto const canonical_good = std::filesystem::weakly_canonical(good);
    EXPECT_EQ(discovered.front().first, canonical_good.generic_string());
    EXPECT_EQ(discovered.front().second, canonical_good);
}

TEST(PackageWatcher, SuccessfulBuildDependencySetCanDirtyPackageWithoutSourceChange)
{
    auto const root = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_external_dependency_source");
    auto const dependency_root = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_external_dependency");
    auto const dependency_file = dependency_root / "dependency.h";
    iv::test_support::write_text(root / "module.cpp", "// package source\n");
    iv::test_support::write_text(dependency_file, "// v1\n");
    auto const package_id = std::string("iv.test.dependency");

    WatcherHarness harness;
    std::uint64_t revision_number = 0;
    harness.jit.respond = [&](iv::PackageJitBatchRequest& request) {
        ASSERT_EQ(request.declarations.size(), 1u);
        auto dependencies = std::vector<iv::ModuleDependency>{iv::ModuleDependency{
            .id = "synthetic-dependency",
            .module_dir = dependency_root,
            .entry_file = dependency_file,
            .package_stamp = std::filesystem::last_write_time(dependency_file),
        }};
        request.result.revisions.push_back(revision_for(
            request.declarations.front(),
            ++revision_number,
            std::move(dependencies)));
    };

    harness.watcher.synchronize_discovered_packages({declaration(package_id, root)});
    ASSERT_TRUE(harness.refresh());
    ASSERT_EQ(harness.jit.calls.size(), 1u);
    EXPECT_FALSE(harness.watcher.has_pending_refresh());

    iv::test_support::write_text(dependency_file, "// v2\n");
    iv::test::advance_write_time(dependency_file);
    harness.watcher.poll_dependency_changes();
    ASSERT_TRUE(harness.watcher.has_pending_refresh());
    ASSERT_TRUE(harness.refresh());
    EXPECT_EQ(harness.jit.calls.size(), 2u);
    EXPECT_EQ(revision_number, 2u);
}

TEST(PackageWatcher, IgnoredBuildMetadataDoesNotCreateSpuriousRefresh)
{
    auto const root = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_ignored_build_metadata");
    iv::test_support::write_text(root / "iv_package.json", R"({"schema":2,"entry":"module.cpp"})");
    iv::test_support::write_text(root / "module.cpp", "// package source\n");

    WatcherHarness harness;
    harness.jit.respond = [](iv::PackageJitBatchRequest& request) {
        ASSERT_EQ(request.declarations.size(), 1u);
        request.result.revisions.push_back(revision_for(request.declarations.front()));
    };
    harness.watcher.synchronize_discovered_packages({
        declaration("iv.test.ignored.metadata", root),
    });
    ASSERT_TRUE(harness.refresh());
    EXPECT_FALSE(harness.watcher.has_pending_refresh());

    iv::test_support::write_text(root / "compile_commands.json", "[]\n");
    iv::test::advance_write_time(root / "compile_commands.json");
    harness.watcher.poll_dependency_changes();

    EXPECT_FALSE(harness.watcher.has_pending_refresh());
    EXPECT_FALSE(harness.refresh());
    EXPECT_EQ(harness.jit.calls.size(), 1u);
}
