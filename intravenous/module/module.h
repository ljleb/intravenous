#pragma once

#include "dsl.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace iv {
    class TypeErasedModule;

    struct ModuleRenderConfig {
        size_t sample_rate = 48000;
        size_t num_channels = 2;
        size_t max_block_frames = 4096;
    };

    class ModuleSystem {
        void* _user_data = nullptr;
        ModuleRenderConfig _render_config;
        Sample* _sample_period = nullptr;
        NodeRef (*_sink_fn)(void*, GraphBuilder&, size_t, size_t) = nullptr;

    public:
        ModuleSystem() = default;

        ModuleSystem(
            void* user_data,
            ModuleRenderConfig render_config,
            Sample* sample_period,
            NodeRef (*sink_fn)(void*, GraphBuilder&, size_t, size_t)
        ) :
            _user_data(user_data),
            _render_config(render_config),
            _sample_period(sample_period),
            _sink_fn(sink_fn)
        {}

        ModuleRenderConfig const& render_config() const
        {
            return _render_config;
        }

        Sample& sample_period() const
        {
            if (!_sample_period) {
                throw std::logic_error("module sample_period is unavailable");
            }

            return *_sample_period;
        }

        NodeRef sink(GraphBuilder& builder, size_t channel, size_t device_id = 0) const
        {
            if (!_sink_fn) {
                throw std::logic_error("module audio sink callback is unavailable");
            }

            return _sink_fn(_user_data, builder, channel, device_id);
        }
    };

    class ModuleContext {
        GraphBuilder* _builder = nullptr;
        ModuleSystem _system;
        TypeErasedModule (*_load_fn)(void*, std::string_view) = nullptr;
        void* _load_user_data = nullptr;

    public:
        ModuleContext(
            GraphBuilder& builder,
            ModuleSystem system = {},
            TypeErasedModule (*load_fn)(void*, std::string_view) = nullptr,
            void* load_user_data = nullptr
        ) :
            _builder(&builder),
            _system(std::move(system)),
            _load_fn(load_fn),
            _load_user_data(load_user_data)
        {}

        GraphBuilder& builder() const
        {
            return *_builder;
        }

        ModuleSystem const& system() const
        {
            return _system;
        }

        TypeErasedModule load(std::string_view path) const;
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

    inline TypeErasedModule ModuleContext::load(std::string_view path) const
    {
        if (!_load_fn) {
            throw std::logic_error(
                "module loader is unavailable in this ModuleContext; cannot load '" + std::string(path) + "'"
            );
        }

        return _load_fn(_load_user_data, path);
    }
}

extern "C" {
    static constexpr uint32_t IV_MODULE_ABI_VERSION_V1 = 1;

    using iv_module_build_fn_v1 = iv::TypeErasedNode (*)(iv::ModuleContext const&);

    struct iv_module_descriptor_v1 {
        uint32_t abi_version;
        iv_module_build_fn_v1 build;
    };

    using iv_get_module_descriptor_fn_v1 = iv_module_descriptor_v1 const* (*)();
}

#if defined(_WIN32)
#define IV_MODULE_EXPORT __declspec(dllexport)
#else
#define IV_MODULE_EXPORT __attribute__((visibility("default")))
#endif

#define IV_EXPORT_MODULE(module_fn) \
    extern "C" IV_MODULE_EXPORT iv_module_descriptor_v1 const* iv_get_module_descriptor_v1() \
    { \
        static iv_module_descriptor_v1 descriptor { \
            IV_MODULE_ABI_VERSION_V1, \
            [](iv::ModuleContext const& context) -> iv::TypeErasedNode { return module_fn(context); }, \
        }; \
        return &descriptor; \
    }
