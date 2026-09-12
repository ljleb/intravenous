#pragma once

#include <intravenous/basic_nodes/weak_type_erased.h>
#include <intravenous/graph/build_types.h>
#include <intravenous/module/abi.h>
#include <intravenous/module/dependency.h>
#include <intravenous/node/compiler_record.h>

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
        bool source_introspection = true;
        bool precompiled_header = true;
        bool clang_time_trace = false;
    };

    class ModuleLoader {
        class Impl;
        std::unique_ptr<Impl> _impl;

    public:
        using LogSink = std::function<void(std::string const&)>;

        struct LoadedDefinition {
            std::vector<ModuleRef> module_refs;
            WeakTypeErasedNode root;
            GraphIntrospectionMetadata introspection;
            std::filesystem::path package_path;
            std::string module_id;
            std::vector<ModuleDependency> dependencies;
            // The configured graph is retained above the compatibility GraphLowerer
            // path so whole-project compilation can consume it directly.
            std::shared_ptr<ConfiguredGraph const> configured_graph;

            LoadedDefinition(
                std::vector<ModuleRef> module_refs_,
                WeakTypeErasedNode root_,
                GraphIntrospectionMetadata introspection_,
                std::filesystem::path package_path_,
                std::string module_id_,
                std::vector<ModuleDependency> dependencies_,
                std::shared_ptr<ConfiguredGraph const> configured_graph_
            );
        };

        struct LoadedNodeType {
            // The ID is stable registry identity.  NodeCodeKey and callbacks
            // belong to the loaded IV package code held alive by module_refs.
            std::string node_type_id;
            details::NodeCompilerRecord compiler_record{};
            std::filesystem::path package_path;
            std::vector<ModuleRef> module_refs;
            std::shared_ptr<ConfiguredGraph const> configured_graph;
        };

        struct LoadedPackage {
            std::vector<LoadedDefinition> definitions;
            std::vector<LoadedNodeType> node_types;
            std::vector<ModuleDependency> dependencies;
            // Opaque ownership of this IV package's ORC resources. Definitions and
            // configured graphs retain it while callbacks or retained LLVM globals
            // from this package can still be referenced.
            ModuleRef package_code{};
        };

        explicit ModuleLoader(
            std::filesystem::path discovery_start = std::filesystem::current_path(),
            std::vector<std::filesystem::path> extra_search_roots = {},
            ModuleLoaderToolchainConfig toolchain = ModuleLoaderToolchainConfig(),
            LogSink log_sink = {}
        );
        ~ModuleLoader();
        ModuleLoader(ModuleLoader&&) noexcept;
        ModuleLoader& operator=(ModuleLoader&&) noexcept;

        ModuleLoader(ModuleLoader const&) = delete;
        ModuleLoader& operator=(ModuleLoader const&) = delete;

        // Compiles and loads one IV package, then configures its iv module definitions
        // against all currently valid packages in the project/search roots. A valid
        // package may publish no iv modules.
        LoadedPackage load_package(
            std::filesystem::path const& package_path
        ) const;

        std::vector<LoadedDefinition> load_package_definitions(
            std::filesystem::path const& package_path
        ) const;

        // Builds finalized O0 LLVM for the IV package without adding it to the shared ORC JIT.
        std::filesystem::path compile_package(
            std::filesystem::path const& package_path
        ) const;

        // Updates compiler/build settings for subsequent package compiles without
        // replacing the shared ORC instance or invalidating loaded package code.
        void set_toolchain_config(ModuleLoaderToolchainConfig toolchain);

        std::vector<std::filesystem::path> const& extra_search_roots() const;
    };
}
