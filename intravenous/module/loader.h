#pragma once

#include "module/dependency.h"
#include "module/module.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace iv {
    class System;

    class ModuleLoader {
        class Impl;
        std::unique_ptr<Impl> _impl;

    public:
        struct LoadedGraph {
            std::vector<std::shared_ptr<void>> module_refs;
            TypeErasedNode root;
            std::filesystem::path module_path;
            std::string module_id;
            std::vector<ModuleDependency> dependencies;
            size_t sink_count = 0;

            LoadedGraph(
                TypeErasedNode root_,
                std::vector<std::shared_ptr<void>> module_refs_,
                std::filesystem::path module_path_,
                std::string module_id_,
                std::vector<ModuleDependency> dependencies_,
                size_t sink_count_
            );
        };

        explicit ModuleLoader(
            std::filesystem::path discovery_start = std::filesystem::current_path(),
            std::vector<std::filesystem::path> extra_search_roots = {}
        );
        ~ModuleLoader();
        ModuleLoader(ModuleLoader&&) noexcept;
        ModuleLoader& operator=(ModuleLoader&&) noexcept;

        ModuleLoader(ModuleLoader const&) = delete;
        ModuleLoader& operator=(ModuleLoader const&) = delete;

        LoadedGraph load_root(std::filesystem::path const& module_path, System& system) const;
        std::vector<std::filesystem::path> const& extra_search_roots() const;
    };
}
