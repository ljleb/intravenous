#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/module/abi.h>
#include <intravenous/module/dependency.h>
#include <intravenous/module/package_compiler_artifact.h>
#include <intravenous/module/package_definitions.h>
#include <intravenous/module/package_provenance.h>
#include <intravenous/module/builder_session.h>
#include <intravenous/node/compiler_record.h>
#include <intravenous/node/presentation.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace iv {
    using ModuleRef = std::shared_ptr<void>;
    struct ConfiguredGraph;

    struct ModuleLoaderToolchainConfig {
        std::optional<std::filesystem::path> c_compiler {};
        std::optional<std::filesystem::path> cxx_compiler {};
        std::optional<std::filesystem::path> cmake_program {};
        std::optional<std::string> cmake_generator {};
        std::optional<std::filesystem::path> make_program {};
        std::optional<std::filesystem::path> juce_dir {};
        // Overrides the application-built DSL PCH. When omitted, a loader
        // built by the application uses its configured shared DSL PCH.
        std::optional<std::filesystem::path> iv_package_pch {};
        bool source_introspection = true;
        bool clang_time_trace = false;
    };

    class ModuleLoader {
        class Impl;
        std::unique_ptr<Impl> _impl;

    public:
        using LogSink = std::function<void(std::string const&)>;

        struct LoadedDefinition {
            std::vector<ModuleRef> module_refs;
            GraphIntrospectionMetadata introspection;
            std::filesystem::path package_path;
            std::string module_id;
            details::PackageDefinition provider{};
            std::vector<ModuleDependency> dependencies;
            // The configured graph is the lossless module product consumed by
            // whole-project compilation.
            std::shared_ptr<ConfiguredGraph const> configured_graph;

            LoadedDefinition(
                std::vector<ModuleRef> module_refs_,
                GraphIntrospectionMetadata introspection_,
                std::filesystem::path package_path_,
                std::string module_id_,
                details::PackageDefinition provider_,
                std::vector<ModuleDependency> dependencies_,
                std::shared_ptr<ConfiguredGraph const> configured_graph_
            );
        };

        struct LoadedNodeType {
            // The ID is stable registry identity.  NodeCodeKey and callbacks
            // belong to the loaded IV package code held alive by module_refs.
            std::string node_type_id;
            details::NodeCompilerRecord compiler_record{};
            details::PackageDefinition provider{};
            std::filesystem::path package_path;
            std::vector<ModuleRef> module_refs;
        };

        struct LoadedPackage {
            std::vector<LoadedDefinition> definitions;
            std::vector<LoadedNodeType> node_types;
            std::vector<ModuleDependency> dependencies;
            // Exact finalized package compiler inputs. These are kept separate
            // from package_code so whole-project compilation does not need to
            // reach through the loader's opaque ORC ownership object.
            PackageCompilerArtifact compiler_artifact{};
            // Complete package-side configuration tables used to create a stable
            // BuilderSession later from an immutable NodeDefinitions snapshot.
            std::vector<details::PackageDefinition> provider_definitions{};
            std::vector<NodePresentationDescriptor> node_presentations{};
            std::vector<NodeConfigPointerFieldData> config_pointer_fields{};
            std::vector<RetainedGlobalData> retained_globals{};
            std::vector<NodeMessageHandlerData> node_message_handlers{};
            std::vector<ModuleMessageHandlerData> module_message_handlers{};
            std::vector<details::BuilderNodeStateStructures> node_state_structures{};
            // The package's node code, compiled once per package build (see
            // NodeCodeRecordData). Valid while package_code is retained.
            std::vector<NodeCodeRecordData> node_code_records{};
            std::vector<NodeCodeRetainedGlobalData> node_code_retained_globals{};
            // Opaque ownership of this IV package's ORC resources. Definitions and
            // configured graphs retain it while callbacks or retained LLVM globals
            // from this package can still be referenced.
            ModuleRef package_code{};
        };

        // One requested package is one candidate in a reload transaction. A
        // candidate can fail while unrelated candidates in the same batch are
        // still configured and returned successfully.
        struct PackageLoadResult {
            std::filesystem::path package_path{};
            std::optional<LoadedPackage> package{};
            std::string error{};

            [[nodiscard]] explicit operator bool() const noexcept
            {
                return package.has_value();
            }
        };

        explicit ModuleLoader(
            std::filesystem::path discovery_start = std::filesystem::current_path(),
            ModuleLoaderToolchainConfig toolchain = ModuleLoaderToolchainConfig(),
            LogSink log_sink = {}
        );
        ~ModuleLoader();
        ModuleLoader(ModuleLoader&&) noexcept;
        ModuleLoader& operator=(ModuleLoader&&) noexcept;

        ModuleLoader(ModuleLoader const&) = delete;
        ModuleLoader& operator=(ModuleLoader const&) = delete;

        // Compiles and loads one IV package, then configures its iv module definitions
        // against the last-valid package revisions already loaded into this
        // loader. A valid package may publish no iv modules. Use load_packages()
        // to establish a fresh multi-package candidate set, including every
        // provider that is not already active.
        LoadedPackage load_package(
            PackageBuildRequest const& package
        ) const;

        LoadedPackage load_project_package(
            std::filesystem::path const& package_path
        ) const;

        // Compiles only `packages`, then configures each successful
        // candidate against that batch and the last valid revisions of all
        // other packages. It deliberately does not scan and rebuild every
        // package discoverable from the project directory.
        std::vector<PackageLoadResult> load_packages(
            std::vector<PackageBuildRequest> const& packages
        ) const;

        // Removes a package from the active definition/configuration set after
        // package discovery says that its source disappeared. Existing graphs
        // retain their ModuleRef-owned code revision until they are replaced.
        void remove_package(std::filesystem::path const& package_path) const;

        std::vector<LoadedDefinition> load_package_definitions(
            PackageBuildRequest const& package
        ) const;

        std::vector<LoadedDefinition> load_project_package_definitions(
            std::filesystem::path const& package_path
        ) const;

        // Builds finalized O0 LLVM for the IV package without adding it to the shared ORC JIT.
        std::filesystem::path compile_package(
            PackageBuildRequest const& package
        ) const;

        std::filesystem::path compile_project_package(
            std::filesystem::path const& package_path
        ) const;

        // Updates compiler/build settings for subsequent package compiles without
        // replacing the shared ORC instance or invalidating loaded package code.
        void set_toolchain_config(ModuleLoaderToolchainConfig toolchain);

    };
}
