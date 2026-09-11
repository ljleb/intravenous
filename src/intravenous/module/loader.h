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
    struct AuthoredGraph;

    enum class ModuleCompileStage {
        full,
        configuration,
        lowering_topology,
        lowering_materialization,
        lowering_normalization,
        lowering,
        compilation,
        static_metadata,
    };

    // The module benchmark can override this while keeping the module build
    // configuration otherwise equivalent to Release.
    enum class ModuleOptimization {
        O0,
        O3,
    };

    struct ModuleLoaderToolchainConfig {
        std::optional<std::filesystem::path> c_compiler {};
        std::optional<std::filesystem::path> cxx_compiler {};
        std::optional<std::filesystem::path> cmake_program {};
        std::optional<std::string> cmake_generator {};
        std::optional<std::filesystem::path> make_program {};
        std::optional<std::filesystem::path> juce_dir {};
        ModuleCompileStage compile_stage = ModuleCompileStage::full;
        ModuleOptimization optimization = ModuleOptimization::O3;
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
            std::filesystem::path source_path;
            std::string module_id;
            std::vector<ModuleDependency> dependencies;
            // The immutable source-authored graph is retained above the
            // compatibility GraphLowerer path so a whole-project finalizer can
            // consume it without reauthoring this source.
            std::shared_ptr<AuthoredGraph const> authored_graph;

            LoadedDefinition(
                std::vector<ModuleRef> module_refs_,
                WeakTypeErasedNode root_,
                GraphIntrospectionMetadata introspection_,
                std::filesystem::path source_path_,
                std::string module_id_,
                std::vector<ModuleDependency> dependencies_,
                std::shared_ptr<AuthoredGraph const> authored_graph_
            );
        };

        struct LoadedNodeType {
            // The ID is stable registry identity.  NodeCodeKey and callbacks
            // are artifact-local compiler data held alive by module_refs.
            std::string node_type_id;
            details::NodeCompilerRecord compiler_record{};
            std::filesystem::path source_path;
            std::vector<ModuleRef> module_refs;
            std::shared_ptr<AuthoredGraph const> authored_graph;
        };

        struct LoadedSource {
            std::vector<LoadedDefinition> definitions;
            std::vector<LoadedNodeType> node_types;
            std::vector<ModuleDependency> dependencies;
            // Opaque ownership of the loaded IV source binary. Definitions and
            // configured graphs retain this while any callback or immutable
            // source-global address from that binary can still be referenced.
            ModuleRef source_binary{};
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

        // Loads one IV source package independently of runtime render
        // configuration. A valid source may publish no IV modules, so source
        // dependencies are reported independently of the definition vector
        // for watching and transactional reload.
        LoadedSource load_source(
            std::filesystem::path const& source_path
        ) const;

        std::vector<LoadedDefinition> load_source_definitions(
            std::filesystem::path const& source_path
        ) const;

        // Builds the generated module artifact without loading it. This is
        // primarily useful for compile-time profiling stages.
        std::filesystem::path compile_source(
            std::filesystem::path const& source_path
        ) const;

        std::vector<std::filesystem::path> const& extra_search_roots() const;
    };
}
