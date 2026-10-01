#include "../module_test_utils.h"

#include <intravenous/bridge.h>
#include <intravenous/linux_file_descriptor.h>
#include <intravenous/runtime/node_instances.h>
#include <intravenous/runtime/package_discovery_watcher.h>
#include <intravenous/runtime/package_pipeline_events.h>
#include <intravenous/runtime/package_watcher.h>
#include <intravenous/runtime/package_watcher_service.h>
#include <intravenous/runtime/package_watcher_service_bridge.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <mutex>
#include <ranges>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/stat.h>

namespace {

bool wait_until_readable(int fd, int timeout_ms = 1000)
{
    pollfd descriptor{.fd = fd, .events = POLLIN, .revents = 0};
    for (;;) {
        auto const result = poll(&descriptor, 1, timeout_ms);
        if (result > 0) return (descriptor.revents & POLLIN) != 0;
        if (result == 0) return false;
        if (errno != EINTR) return false;
    }
}

std::optional<std::uint64_t> inotify_queue_limit()
{
    std::ifstream input("/proc/sys/fs/inotify/max_queued_events");
    std::uint64_t value = 0;
    if (!(input >> value) || value == 0) return std::nullopt;
    return value;
}

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

struct FakePackageWatcherWorkSink {
    int calls = 0;

    void handle_work_available(iv::PackageWatcherWorkAvailable const&)
    {
        ++calls;
    }
};

IV_DECLARE_BRIDGE(
    package_watcher_fake_work_bridge,
    iv::PackageWatcher,
    FakePackageWatcherWorkSink);
IV_DEFINE_BRIDGE(package_watcher_fake_work_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    package_watcher_fake_work_bridge,
    iv_runtime_package_watcher_work_available_event,
    &FakePackageWatcherWorkSink::handle_work_available)

struct ServiceCatalogObserver {
    std::mutex mutex{};
    std::condition_variable changed{};
    std::size_t calls = 0;

    void handle_catalog_changed(iv::IvPackageCatalogChanged const&)
    {
        {
            std::scoped_lock lock(mutex);
            ++calls;
        }
        changed.notify_all();
    }

    bool wait_for_calls(std::size_t expected, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return calls >= expected; });
    }

    std::size_t call_count()
    {
        std::scoped_lock lock(mutex);
        return calls;
    }
};

IV_DECLARE_BRIDGE(
    package_watcher_service_catalog_observer_bridge,
    iv::PackageWatcherService,
    ServiceCatalogObserver);
IV_DEFINE_BRIDGE(package_watcher_service_catalog_observer_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    package_watcher_service_catalog_observer_bridge,
    iv_runtime_iv_package_catalog_changed_event,
    &ServiceCatalogObserver::handle_catalog_changed)

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

TEST(EventSignal, BurstSignalsCoalesceWithoutLosingFutureWakeups)
{
    iv::EventSignal signal("event signal test");

    for (int i = 0; i < 1024; ++i) signal.signal();
    ASSERT_TRUE(wait_until_readable(signal.native_handle()));

    signal.consume();
    EXPECT_FALSE(wait_until_readable(signal.native_handle(), 0));

    // Consuming an accumulated burst must re-arm the descriptor for a later
    // wakeup rather than leaving it permanently readable or permanently empty.
    signal.signal();
    EXPECT_TRUE(wait_until_readable(signal.native_handle()));
    signal.consume();
    EXPECT_FALSE(wait_until_readable(signal.native_handle(), 0));
}

TEST(PackageDiscoveryWatcher, AddsCoverageForNewNestedDirectories)
{
    auto const root = iv::test_support::fresh_module_fixture_workspace(
        "package_discovery_watcher_nested");
    iv::PackageDiscoveryWatcher watcher({root});

    auto const package_root = root / "new";
    std::filesystem::create_directory(package_root);
    ASSERT_TRUE(wait_until_readable(watcher.native_handle()));
    auto events = watcher.consume_events();
    EXPECT_TRUE(events.coverage_may_have_changed);
    EXPECT_TRUE(events.declarations_may_have_changed);
    watcher.refresh_coverage();

    auto const nested = package_root / "nested";
    std::filesystem::create_directory(nested);
    ASSERT_TRUE(wait_until_readable(watcher.native_handle()));
    events = watcher.consume_events();
    EXPECT_TRUE(events.coverage_may_have_changed);
    watcher.refresh_coverage();

    iv::test_support::write_text(
        nested / "iv_package.json",
        R"({"schema":2,"entry":"module.cpp"})");
    ASSERT_TRUE(wait_until_readable(watcher.native_handle()));
    events = watcher.consume_events();
    EXPECT_TRUE(events.declarations_may_have_changed);
}

TEST(PackageDiscoveryWatcher, WatchesNearestExistingAncestorForMissingRootAndPrunesIt)
{
    auto const workspace = iv::test_support::fresh_module_fixture_workspace(
        "package_discovery_watcher_missing_root");
    auto const missing_root = workspace / "shared" / "packages";
    iv::PackageDiscoveryWatcher watcher({missing_root});

    std::filesystem::create_directory(workspace / "shared");
    ASSERT_TRUE(wait_until_readable(watcher.native_handle()));
    auto events = watcher.consume_events();
    ASSERT_TRUE(events.coverage_may_have_changed);
    watcher.refresh_coverage();

    // refresh_coverage() migrates the watch from workspace to shared. Drain the
    // IN_IGNORED generated by pruning that old watch before checking isolation.
    if (wait_until_readable(watcher.native_handle(), 0)) {
        (void)watcher.consume_events();
    }
    std::filesystem::create_directory(workspace / "unrelated");
    EXPECT_FALSE(wait_until_readable(watcher.native_handle(), 50));

    std::filesystem::create_directory(missing_root);
    EXPECT_TRUE(wait_until_readable(watcher.native_handle()));
}

TEST(PackageDiscoveryWatcher, ExistingSourceWritesDoNotRequestDiscoveryRescan)
{
    auto const root = iv::test_support::fresh_module_fixture_workspace(
        "package_discovery_watcher_event_filter");
    auto const source = root / "module.cpp";
    auto const manifest = root / "iv_package.json";
    iv::test_support::write_text(source, "int value = 1;\n");
    iv::test_support::write_text(manifest, R"({"schema":2,"entry":"module.cpp"})");

    iv::PackageDiscoveryWatcher watcher({root});

    iv::test_support::write_text(source, "int value = 2;\n");
    ASSERT_TRUE(wait_until_readable(watcher.native_handle()));
    auto events = watcher.consume_events();
    EXPECT_FALSE(events.coverage_may_have_changed);
    EXPECT_FALSE(events.declarations_may_have_changed);

    iv::test_support::write_text(manifest, R"({"schema":2,"entry":"module.cpp"})");
    ASSERT_TRUE(wait_until_readable(watcher.native_handle()));
    events = watcher.consume_events();
    EXPECT_FALSE(events.coverage_may_have_changed);
    EXPECT_TRUE(events.declarations_may_have_changed);
}

TEST(PackageDiscoveryWatcher, QueueOverflowForcesConservativeReconciliationAndRestoresCoverage)
{
    auto const limit = inotify_queue_limit();
    if (!limit.has_value()) {
        GTEST_SKIP() << "cannot read /proc/sys/fs/inotify/max_queued_events";
    }
    // Keep a machine-wide tuning choice from turning this one adversarial test
    // into hundreds of thousands of syscalls in every light-suite run.
    if (*limit > 131072) {
        GTEST_SKIP() << "inotify queue limit is too large for a bounded overflow test: "
                     << *limit;
    }

    auto const root = iv::test_support::fresh_module_fixture_workspace(
        "package_discovery_watcher_queue_overflow");
    auto const first = root / "overflow-first.cpp";
    auto const second = root / "overflow-second.cpp";
    iv::test_support::write_text(first, "int first = 1;\n");
    iv::test_support::write_text(second, "int second = 2;\n");

    // Both instances receive the same flood. Drain one as a probe before any
    // topology mutation: IN_ATTRIB on an ordinary source file is deliberately
    // filtered, so only IN_Q_OVERFLOW can make either discovery flag true.
    iv::PackageDiscoveryWatcher overflow_probe({root});
    iv::PackageDiscoveryWatcher watcher({root});
    auto const event_count = *limit + 1024;
    for (std::uint64_t i = 0; i < event_count; ++i) {
        auto const& path = (i % 2 == 0) ? first : second;
        auto const mode = ((i / 2) % 2 == 0) ? mode_t{0600} : mode_t{0700};
        ASSERT_EQ(::chmod(path.c_str(), mode), 0);
    }

    ASSERT_TRUE(wait_until_readable(overflow_probe.native_handle()));
    auto probe_events = overflow_probe.consume_events();
    ASSERT_TRUE(probe_events.coverage_may_have_changed);
    ASSERT_TRUE(probe_events.declarations_may_have_changed)
        << "the non-coalescing IN_ATTRIB flood did not overflow the kernel queue";

    // The second instance is still full. Build an entire package subtree while
    // its queue is overflowing, so correctness cannot depend on replaying the
    // CREATE events that establish recursive coverage.
    auto const package_root = root / "created-during-overflow" / "nested";
    std::filesystem::create_directories(package_root);
    auto const manifest = package_root / "iv_package.json";
    iv::test_support::write_text(
        manifest,
        R"({"schema":2,"entry":"module.cpp"})");
    iv::test_support::write_text(
        package_root / "module.cpp",
        "// discovered after overflow\n");

    ASSERT_TRUE(wait_until_readable(watcher.native_handle()));
    auto events = watcher.consume_events();
    EXPECT_TRUE(events.coverage_may_have_changed);
    EXPECT_TRUE(events.declarations_may_have_changed);

    // This is the service's overflow recovery order: reconstruct recursive
    // coverage first, then derive package state from the filesystem snapshot.
    watcher.refresh_coverage();
    auto const discovered = iv::discover_iv_package_declarations(root, {});
    auto const canonical_package_root = std::filesystem::weakly_canonical(package_root);
    ASSERT_EQ(discovered.size(), 1u);
    EXPECT_EQ(discovered.front().second, canonical_package_root);

    // Recovery must also re-arm future observation below the subtree whose
    // creation events were dropped, not merely reconstruct this one snapshot.
    iv::test_support::write_text(
        manifest,
        R"({"schema":2,"entry":"module.cpp"})");
    ASSERT_TRUE(wait_until_readable(watcher.native_handle()));
    events = watcher.consume_events();
    EXPECT_TRUE(events.declarations_may_have_changed);
}

TEST(PackageDiscoveryWatcher, RepeatedRootMoveAndRecreationDoesNotLoseCoverage)
{
    auto const workspace = iv::test_support::fresh_module_fixture_workspace(
        "package_discovery_watcher_root_recreation");
    auto const root = workspace / "packages";
    std::filesystem::create_directories(root);
    iv::PackageDiscoveryWatcher watcher({root});

    for (int cycle = 0; cycle < 3; ++cycle) {
        auto const moved = workspace / ("packages-moved-" + std::to_string(cycle));
        std::filesystem::rename(root, moved);

        ASSERT_TRUE(wait_until_readable(watcher.native_handle()));
        auto events = watcher.consume_events();
        EXPECT_TRUE(events.coverage_may_have_changed);
        EXPECT_TRUE(events.declarations_may_have_changed);
        watcher.refresh_coverage();

        // refresh_coverage() prunes watches attached to the moved inode. Drain
        // the corresponding retired IN_IGNORED generation before exercising the
        // new ancestor watch.
        if (wait_until_readable(watcher.native_handle(), 0)) {
            (void)watcher.consume_events();
        }

        auto const package_root = root / "nested";
        std::filesystem::create_directories(package_root);
        auto const manifest = package_root / "iv_package.json";
        iv::test_support::write_text(
            manifest,
            R"({"schema":2,"entry":"module.cpp"})");
        iv::test_support::write_text(package_root / "module.cpp", "// recreated package\n");

        ASSERT_TRUE(wait_until_readable(watcher.native_handle()));
        events = watcher.consume_events();
        EXPECT_TRUE(events.coverage_may_have_changed);
        EXPECT_TRUE(events.declarations_may_have_changed);

        // The manifest was created before coverage could migrate from the
        // ancestor to the recreated tree. Reconciliation + durable rescan must
        // still find it, and subsequent manifest writes must be watched.
        watcher.refresh_coverage();
        auto const discovered = iv::discover_iv_package_declarations(root, {});
        ASSERT_EQ(discovered.size(), 1u);
        EXPECT_EQ(
            discovered.front().second,
            std::filesystem::weakly_canonical(package_root));

        if (wait_until_readable(watcher.native_handle(), 0)) {
            (void)watcher.consume_events();
        }
        iv::test_support::write_text(
            manifest,
            R"({"schema":2,"entry":"module.cpp"})");
        ASSERT_TRUE(wait_until_readable(watcher.native_handle()));
        events = watcher.consume_events();
        EXPECT_TRUE(events.declarations_may_have_changed);

        std::filesystem::remove_all(moved);
    }
}


TEST(PackageWatcherService, LogicalWorkAfterStartupIsNotStrandedWithoutFilesystemActivity)
{
    using namespace std::chrono_literals;

    auto const project_root = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_service_project");
    auto const initial_package = project_root / "initial";
    std::filesystem::create_directories(initial_package);
    iv::test_support::write_text(
        initial_package / "iv_package.json",
        R"({"schema":2,"entry":"module.cpp"})");
    iv::test_support::write_text(initial_package / "module.cpp", "// initial\n");

    // This package is intentionally outside every discovery root and is fully
    // created before the service starts. Adding it later through required-
    // definition state therefore produces no filesystem event the service can
    // accidentally rely on.
    auto const retained_package = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_service_retained");

    iv::PackageWatcher watcher;
    ServiceCatalogObserver observer;
    // Keep bridge bindings alive until after the worker has joined, including
    // assertion-failure unwinding. The service is deliberately declared after
    // the optional scopes so its destructor runs first.
    std::optional<iv::package_watcher_service_bridge::scope> service_scope;
    std::optional<package_watcher_service_catalog_observer_bridge::scope> observer_scope;
    iv::PackageWatcherService service(watcher, project_root, {});
    service_scope = iv::package_watcher_service_bridge::bind(service, watcher);
    observer_scope = package_watcher_service_catalog_observer_bridge::bind(
        service,
        observer);

    service.start();
    ASSERT_TRUE(observer.wait_for_calls(1, 2s))
        << "startup package discovery never completed";
    auto const startup_calls = observer.call_count();

    watcher.handle_required_definitions_changed(iv::IvModuleRequiredDefinitionsChanged{
        .created = {iv::IvModuleRequiredDefinition{
            .definition_id = "iv.test.service.retained",
            .package_root = retained_package,
        }},
    });

    ASSERT_TRUE(observer.wait_for_calls(startup_calls + 1, 2s))
        << "logical package work was stranded after the polling timer was removed";
    service.request_shutdown();
}

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

TEST(PackageWatcher, RequiredDefinitionChangePublishesWorkAvailable)
{
    auto const root = iv::test_support::fresh_module_fixture_workspace(
        "package_watcher_work_signal");
    WatcherHarness harness;
    FakePackageWatcherWorkSink sink;
    auto work_scope = package_watcher_fake_work_bridge::bind(harness.watcher, sink);

    harness.watcher.handle_required_definitions_changed(iv::IvModuleRequiredDefinitionsChanged{
        .created = {iv::IvModuleRequiredDefinition{
            .definition_id = "iv.test.work_signal",
            .package_root = root,
        }},
    });

    EXPECT_EQ(sink.calls, 1);
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
