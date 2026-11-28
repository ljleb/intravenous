#include "test_support.h"

// Authored package source and its shared build/load path. CTest builds this
// package once before running the independent GraphJitRuntimeFixture tests.

namespace iv::test::graph_jit_tests {

namespace {

constexpr char graph_jit_state_package_id[] = "iv.test.graph_jit.state_context.package";

constexpr char graph_jit_runtime_fixture_name[] = "graph_jit_runtime_shared_package";

} // namespace

std::shared_ptr<iv::PackageRevision const> load_graph_jit_runtime_revision()
{
    auto const workspace = std::filesystem::weakly_canonical(
        iv::test::read_only_module_fixture_workspace(graph_jit_runtime_fixture_name));
    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    iv::PackageJit package_jit(startup_config.initialize());
    iv::PackageJitBatchRequest request{
        .declarations = {iv::IvPackageDeclaration{
            .package_id = graph_jit_state_package_id,
            .package_root = workspace,
            // Each test has its own managed cache. This artifact belongs to
            // the shared fixture that CTest prepares before those tests.
            .artifact_root = workspace / "build/iv",
        }},
    };
    package_jit.handle_build_request(request);
    if (!request.result.failed.empty()) {
        throw std::runtime_error(request.result.failed.front().message);
    }
    if (request.result.revisions.size() != 1u) {
        throw std::runtime_error("GraphJit shared runtime fixture did not produce one package revision");
    }
    return std::make_shared<iv::PackageRevision const>(
        std::move(request.result.revisions.front()));
}

} // namespace iv::test::graph_jit_tests
