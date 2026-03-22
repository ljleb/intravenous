#pragma once

#include "dsl.h"

#include <memory>
#include <type_traits>
#include <utility>

namespace iv {
    class System;

    class ModuleContext {
        GraphBuilder* _builder;
        System* _system;

    public:
        ModuleContext(GraphBuilder& builder, System& system) :
            _builder(&builder),
            _system(&system)
        {}

        GraphBuilder& builder() const
        {
            return *_builder;
        }

        System& system() const
        {
            return *_system;
        }
    };

    class TypeErasedModule {
        std::shared_ptr<void> _module;
        TypeErasedNode (*_build_fn)(void*, ModuleContext const&) = nullptr;

    public:
        TypeErasedModule() = default;

        template<typename Module>
        explicit TypeErasedModule(Module module)
        {
            using StoredModule = std::decay_t<Module>;
            if constexpr (std::is_empty_v<StoredModule>) {
                _module = nullptr;
                _build_fn = [](void*, ModuleContext const& context) {
                    return StoredModule{}(context);
                };
            } else {
                _module = std::make_shared<StoredModule>(std::move(module));
                _build_fn = [](void* module_ptr, ModuleContext const& context) {
                    return (*static_cast<StoredModule*>(module_ptr))(context);
                };
            }
        }

        TypeErasedNode build(ModuleContext const& context) const
        {
            return _build_fn(_module.get(), context);
        }
    };
}

extern "C" {
    struct iv_render_config_v1 {
        size_t sample_rate;
        size_t num_channels;
        size_t max_block_frames;
    };

    struct iv_module_context_v1 {
        void* builder;
        void* system;
        float* sample_period;
        iv_render_config_v1 render_config;
    };

    using iv_create_module_fn_v1 = void* (*)(iv_module_context_v1 const*);
}
