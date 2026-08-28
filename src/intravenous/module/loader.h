#pragma once

#include <intravenous/basic_nodes/weak_type_erased.h>
#include <intravenous/graph/build_types.h>
#include <intravenous/module/abi.h>
#include <intravenous/module/dependency.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace iv {
    using ModuleRef = std::shared_ptr<void>;

    enum class ModuleCompileStage {
        full,
        parse,
        authoring,
        metadata,
        execution_reflection,
        // Finalize and freeze the Graph value, but do not instantiate
        // StaticGraphRoot<Graph>. This separates builder constant evaluation
        // from the deliberately static execution-code generation.
        execution_graph,
        execution,
    };

    struct ModuleLoaderToolchainConfig {
        std::optional<std::filesystem::path> c_compiler {};
        std::optional<std::filesystem::path> cxx_compiler {};
        std::optional<std::filesystem::path> cmake_program {};
        std::optional<std::string> cmake_generator {};
        std::optional<std::filesystem::path> make_program {};
        std::optional<std::filesystem::path> juce_dir {};
        bool gcc_time_report = false;
        ModuleCompileStage compile_stage = ModuleCompileStage::full;
        bool source_introspection = true;
        bool precompiled_header = true;
        // Empty keeps GCC's default constexpr evaluator cache depth.
        std::optional<size_t> constexpr_cache_depth {};
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
            std::filesystem::path module_path;
            std::string module_id;
            std::vector<ModuleDependency> dependencies;

            LoadedDefinition(
                std::vector<ModuleRef> module_refs_,
                WeakTypeErasedNode root_,
                GraphIntrospectionMetadata introspection_,
                std::filesystem::path module_path_,
                std::string module_id_,
                std::vector<ModuleDependency> dependencies_
            );
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

        // Loading a graph definition is deliberately independent of runtime
        // render configuration. Sample rate and other device/runtime values are
        // supplied only when DSP nodes execute through TickContext.
        LoadedDefinition load_root_definition(
            std::filesystem::path const& module_path
        ) const;

        // Builds the generated module artifact without loading it. This is
        // primarily useful for compile-time profiling stages.
        std::filesystem::path compile_root_definition(
            std::filesystem::path const& module_path
        ) const;

        std::vector<std::filesystem::path> const& extra_search_roots() const;
    };
}
