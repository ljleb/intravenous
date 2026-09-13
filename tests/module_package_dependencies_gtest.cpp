#include "module_test_utils.h"

#include <gtest/gtest.h>

#include <string_view>

namespace {
std::filesystem::path finalized_package_bitcode(std::filesystem::path const& workspace)
{
    std::vector<std::filesystem::path> artifacts;
    for (std::filesystem::recursive_directory_iterator it(workspace / "build/iv"), end;
         it != end;
         ++it) {
        if (it->is_regular_file()
            && it->path().filename().string().ends_with(".ivpkg.bc")) {
            artifacts.push_back(it->path());
        }
    }
    EXPECT_EQ(artifacts.size(), 1u);
    return artifacts.empty() ? std::filesystem::path{} : artifacts.front();
}

std::vector<std::string> nonempty_manifest_lines(std::filesystem::path const& manifest)
{
    std::vector<std::string> result;
    std::istringstream input(iv::test::read_text(manifest));
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) result.push_back(std::move(line));
    }
    return result;
}

template<typename Fn>
void expect_failure_contains(Fn&& fn, std::string_view needle)
{
    try {
        fn();
    } catch (std::exception const& error) {
        EXPECT_NE(std::string_view(error.what()).find(needle), std::string_view::npos)
            << error.what();
        return;
    }
    FAIL() << "expected exception containing: " << needle;
}
}

TEST(ModulePackageDependencies, DeferredCustomCmakeLinksStaticAndDynamicDependencies)
{
    auto const workspace = iv::test::mutable_module_fixture_workspace(
        "package_dependencies_deferred_cmake", "package_dependencies");

    auto loader = iv::test::make_loader();
    auto loaded = loader.load_package_definitions(workspace);
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().module_id, "iv.test.package_dependencies");
    EXPECT_TRUE(static_cast<bool>(loaded.front().root));

    // The module builder calls functions from the transitive static archive and
    // from the shared library. A successful configuration proves that deferred
    // target-link collection, archive extraction, and ORC symbol generation all
    // participated; neither function is compiled into module.cpp itself.
    auto const artifact = finalized_package_bitcode(workspace);
    ASSERT_FALSE(artifact.empty());
    auto dynamic_manifest = artifact;
    dynamic_manifest += ".dynamic-libraries";
    ASSERT_TRUE(std::filesystem::is_regular_file(dynamic_manifest));
    auto const dynamic_libraries = nonempty_manifest_lines(dynamic_manifest);
    ASSERT_EQ(dynamic_libraries.size(), 1u);
    EXPECT_TRUE(std::filesystem::is_regular_file(
        std::filesystem::path(dynamic_libraries.front())));
    EXPECT_NE(dynamic_libraries.front().find("iv_test_dynamic"), std::string::npos);

    // The declared shared dependency is an ORC provider, not an IV-package
    // DSO. It may exist in the custom CMake build tree, but must never appear
    // beside the finalized package artifact.
    for (std::filesystem::recursive_directory_iterator it(artifact.parent_path()), end;
         it != end;
         ++it) {
        if (!it->is_regular_file()) continue;
        auto const extension = it->path().extension().string();
        EXPECT_FALSE(
            extension == ".so" || extension == ".dylib" || extension == ".dll")
            << it->path();
    }

    auto const compile_database = iv::test::read_text(
        iv::test::runtime_module_workspace(workspace)
        / "cmake-build/compile_commands.json");
    EXPECT_NE(compile_database.find("lto_root.cpp"), std::string::npos);
    EXPECT_NE(compile_database.find("-flto=full"), std::string::npos);
}

TEST(ModulePackageDependencies, DynamicLibraryManifestAdversarialCorpus)
{
    auto const workspace = iv::test::mutable_module_fixture_workspace(
        "package_dependencies_manifest_corpus", "package_dependencies");
    auto loader = iv::test::make_loader();
    ASSERT_NO_THROW((void)loader.load_package_definitions(workspace));

    auto const artifact = finalized_package_bitcode(workspace);
    ASSERT_FALSE(artifact.empty());
    auto manifest = artifact;
    manifest += ".dynamic-libraries";
    auto const original = nonempty_manifest_lines(manifest);
    ASSERT_EQ(original.size(), 1u);

    // Blank lines, CRLF, and duplicate declarations are harmless. This avoids
    // letting a platform-specific file writer turn one library into multiple
    // ORC generators.
    iv::test::write_text(
        manifest,
        "\r\n" + original.front() + "\r\n" + original.front() + "\n\n");
    auto duplicate_loader = iv::test::make_loader();
    ASSERT_NO_THROW((void)duplicate_loader.load_package_definitions(workspace));

    // Fuzz a compact deterministic corpus of hostile-but-plausible paths. The
    // loader must fail closed before it creates a JITDylib or loads any code.
    std::vector<std::string> const malformed_entries{
        "missing-library.so\n",
        " ./leading-space-library.so\n",
        "../../escape-from-artifact-directory.so\n",
        ".\n",
        "missing-library-with-crlf.so\r\n",
    };
    for (auto const& entry : malformed_entries) {
        iv::test::write_text(manifest, entry);
        auto malformed_loader = iv::test::make_loader();
        expect_failure_contains(
            [&] { (void)malformed_loader.load_package_definitions(workspace); },
            "IV package dynamic library listed");
    }
}

TEST(ModulePackageDependencies, NativeStaticArchiveWithoutBitcodeFailsClosed)
{
    auto const workspace = iv::test::mutable_module_fixture_workspace(
        "package_dependencies_native_archive", "package_dependencies");
    auto cmake = iv::test::read_text(workspace / "CMakeLists.txt");
    auto const insertion = std::string("if(IV_TEST_INCLUDE_NATIVE_ARCHIVE)");
    auto const position = cmake.find(insertion);
    ASSERT_NE(position, std::string::npos);
    cmake.insert(position, "set(IV_TEST_INCLUDE_NATIVE_ARCHIVE ON)\n");
    iv::test::write_text_advancing_timestamp(workspace / "CMakeLists.txt", cmake);

    auto loader = iv::test::make_loader();
    // The package's own LLVM object is valid. This must fail specifically
    // because the static library is a native archive rather than silently
    // discarding that requested dependency as the old finalizer did.
    expect_failure_contains(
        [&] { (void)loader.load_package_definitions(workspace); },
        "command failed");
}
