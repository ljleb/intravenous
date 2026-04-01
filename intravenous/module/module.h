#pragma once

#include "dsl.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace iv {
    class TypeErasedModule;
    class LoadedModule;

    struct ModuleRenderConfig {
        // TODO: This is still a bootstrap/runtime hint passed into module loading.
        // The long-term source of truth should come from executor-target negotiation.
        size_t sample_rate = 48000;
        size_t num_channels = 2;
        size_t max_block_frames = 4096;
    };

    class ModuleTargetFactory {
        void* _user_data = nullptr;
        NodeRef (*_sink_fn)(void*, GraphBuilder&, size_t, size_t) = nullptr;
        NodeRef (*_file_fn)(void*, GraphBuilder&, size_t, std::filesystem::path const&) = nullptr;

    public:
        ModuleTargetFactory() = default;

        ModuleTargetFactory(
            void* user_data,
            NodeRef (*sink_fn)(void*, GraphBuilder&, size_t, size_t),
            NodeRef (*file_fn)(void*, GraphBuilder&, size_t, std::filesystem::path const&)
        ) :
            _user_data(user_data),
            _sink_fn(sink_fn),
            _file_fn(file_fn)
        {}

        NodeRef sink(GraphBuilder& builder, size_t channel, size_t device_id = 0) const
        {
            if (!_sink_fn) {
                throw std::logic_error("module audio sink callback is unavailable");
            }

            return _sink_fn(_user_data, builder, channel, device_id);
        }

        NodeRef file(GraphBuilder& builder, size_t channel, std::filesystem::path const& path) const
        {
            if (!_file_fn) {
                throw std::logic_error("module file target callback is unavailable");
            }

            return _file_fn(_user_data, builder, channel, path);
        }
    };

    class ModuleContext {
        GraphBuilder* _builder = nullptr;
        ModuleTargetFactory _target_factory;
        ModuleRenderConfig _render_config;
        Sample* _sample_period = nullptr;
        TypeErasedModule (*_load_fn)(void*, std::string_view) = nullptr;
        void* _load_user_data = nullptr;

    public:
        ModuleContext(
            GraphBuilder& builder,
            ModuleTargetFactory target_factory = {},
            ModuleRenderConfig render_config = {},
            Sample* sample_period = nullptr,
            TypeErasedModule (*load_fn)(void*, std::string_view) = nullptr,
            void* load_user_data = nullptr
        ) :
            _builder(&builder),
            _target_factory(std::move(target_factory)),
            _render_config(render_config),
            _sample_period(sample_period),
            _load_fn(load_fn),
            _load_user_data(load_user_data)
        {}

        GraphBuilder& builder() const
        {
            return *_builder;
        }

        ModuleTargetFactory const& target_factory() const
        {
            return _target_factory;
        }

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

        TypeErasedModule (*load_fn() const)(void*, std::string_view)
        {
            return _load_fn;
        }

        void* load_user_data() const
        {
            return _load_user_data;
        }

        LoadedModule load(std::string_view id) const;
    };

    class TypeErasedModule {
        std::shared_ptr<void> _module;
        void (*_build_fn)(void*, ModuleContext const&) = nullptr;

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

        GraphBuilder builder(ModuleContext const& context) const
        {
            GraphBuilder builder = context.builder().derive_nested_builder();
            ModuleContext isolated_context(
                builder,
                context.target_factory(),
                context.render_config(),
                &context.sample_period(),
                context.load_fn(),
                context.load_user_data()
            );
            _build_fn(_module.get(), isolated_context);
            return builder;
        }
    };

    class LoadedModule {
        TypeErasedModule _module;
        ModuleContext _context;

    public:
        LoadedModule(TypeErasedModule module, ModuleContext context) :
            _module(std::move(module)),
            _context(std::move(context))
        {
        }

        GraphBuilder builder() const
        {
            return _module.builder(_context);
        }
    };

    inline LoadedModule ModuleContext::load(std::string_view id) const
    {
        if (!_load_fn) {
            throw std::logic_error(
                "module loader is unavailable in this ModuleContext; cannot load '" + std::string(id) + "'"
            );
        }

        return LoadedModule(_load_fn(_load_user_data, id), *this);
    }
}

extern "C" {
    [[maybe_unused]] static constexpr uint32_t IV_MODULE_ABI_VERSION_V1 = 1;

    using iv_module_build_fn_v1 = char const* (*)(iv::ModuleContext const&);

    struct iv_module_descriptor_v1 {
        uint32_t abi_version;
        char const* id;
        iv_module_build_fn_v1 build;
    };

    using iv_get_module_descriptor_fn_v1 = iv_module_descriptor_v1 const* (*)();
}

#if defined(_WIN32)
#define IV_MODULE_EXPORT __declspec(dllexport)
#else
#define IV_MODULE_EXPORT __attribute__((visibility("default")))
#endif

#define IV_EXPORT_MODULE(module_id, module_fn) \
    extern "C" IV_MODULE_EXPORT iv_module_descriptor_v1 const* iv_get_module_descriptor_v1() \
    { \
        static thread_local char iv_module_last_error[2048]; \
        static iv_module_descriptor_v1 descriptor { \
            IV_MODULE_ABI_VERSION_V1, \
            module_id, \
            [](iv::ModuleContext const& context) -> char const* { \
                try { \
                    iv_module_last_error[0] = '\0'; \
                    module_fn(context); \
                    return nullptr; \
                } catch (std::exception const& e) { \
                    std::snprintf(iv_module_last_error, sizeof(iv_module_last_error), "%s", e.what()); \
                    return iv_module_last_error; \
                } catch (...) { \
                    std::snprintf(iv_module_last_error, sizeof(iv_module_last_error), "%s", "non-std exception"); \
                    return iv_module_last_error; \
                } \
            }, \
        }; \
        return &descriptor; \
    }
