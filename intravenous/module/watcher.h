#pragma once

#include "module/dependency.h"

#include <memory>
#include <vector>

namespace iv {
    class DependencyWatcher {
    public:
        virtual ~DependencyWatcher() = default;
        virtual void update(std::vector<ModuleDependency> dependencies) = 0;
        virtual bool has_changes() = 0;
    };

    std::unique_ptr<DependencyWatcher> make_dependency_watcher();
}
