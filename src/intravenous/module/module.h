#pragma once

#include <intravenous/graph/builder.h>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace iv {
    class TypeErasedModule;
    struct ModuleExecutorTarget { size_t sample_rate = 48000; size_t num_channels = 2; size_t max_block_frames = 4096; };
    class ModuleContext {
        GraphBuilder* _builder = nullptr;
        ModuleExecutorTarget _render_config;
        Sample* _sample_period = nullptr;
        TypeErasedModule (*_load_fn)(void*, std::string_view) = nullptr;
        void* _load_user_data = nullptr;
    public:
        ModuleContext(GraphBuilder& builder, ModuleExecutorTarget render_config = {}, Sample* sample_period = nullptr,
            TypeErasedModule (*load_fn)(void*, std::string_view) = nullptr, void* load_user_data = nullptr)
            : _builder(&builder), _render_config(render_config), _sample_period(sample_period), _load_fn(load_fn), _load_user_data(load_user_data) {}
        GraphBuilder& builder() const { return *_builder; }
        ModuleExecutorTarget const& render_config() const { return _render_config; }
        Sample& sample_period() const { if (!_sample_period) throw std::logic_error("module sample_period is unavailable"); return *_sample_period; }
        TypeErasedModule (*load_fn() const)(void*, std::string_view) { return _load_fn; }
        void* load_user_data() const { return _load_user_data; }
        NodeRef load(std::string_view id) const;
        GraphBuilder load_builder(std::string_view id) const;
    };

    class TypeErasedModule {
        std::shared_ptr<void> _module;
        void (*_build_fn)(void*, ModuleContext const&) = nullptr;
    public:
        TypeErasedModule() = default;
        template<typename Module> explicit TypeErasedModule(Module module) {
            using Stored = std::decay_t<Module>;
            if constexpr (std::is_empty_v<Stored>) { _build_fn = [](void*, ModuleContext const& c) { Stored{}(c); }; }
            else { _module = std::make_shared<Stored>(std::move(module)); _build_fn = [](void* p, ModuleContext const& c) { (*static_cast<Stored*>(p))(c); }; }
        }
        GraphBuilder builder(ModuleContext const& context) const {
            GraphBuilder builder = context.builder().derive_nested_builder();
            ModuleContext isolated(builder, context.render_config(), &context.sample_period(), context.load_fn(), context.load_user_data());
            _build_fn(_module.get(), isolated);
            return builder;
        }
    };
    inline GraphBuilder ModuleContext::load_builder(std::string_view id) const {
        if (!_load_fn) throw std::logic_error("module loader is unavailable in this ModuleContext; cannot load '" + std::string(id) + "'");
        return _load_fn(_load_user_data, id).builder(*this);
    }
    inline NodeRef ModuleContext::load(std::string_view id) const { return _builder->embed_subgraph(load_builder(id)); }
}

extern "C" {
    [[maybe_unused]] static constexpr uint32_t IV_MODULE_ABI_VERSION_V1 = 1;
    using iv_module_build_fn_v1 = char const* (*)(iv::ModuleContext const&);
    struct iv_module_descriptor_v1 { uint32_t abi_version; char const* id; iv_module_build_fn_v1 build; };
    using iv_get_module_descriptor_fn_v1 = iv_module_descriptor_v1 const* (*)();
}
#if defined(_WIN32)
#define IV_MODULE_EXPORT __declspec(dllexport)
#else
#define IV_MODULE_EXPORT __attribute__((visibility("default")))
#endif

// Source-compatibility shim only. The loader never discovers or exports from
// this macro in Phase 2; iv_module.json is authoritative. Existing downstream
// sources therefore keep compiling while they remove the obsolete line.
#define IV_EXPORT_MODULE(...)

namespace iv::details {
    template<auto Main>
    void invoke_generated_main(ModuleContext const& context) {
        if constexpr (std::invocable<decltype(Main), GraphBuilder&>) {
            static_assert(std::same_as<std::invoke_result_t<decltype(Main), GraphBuilder&>, void>, "module main must return void");
            Main(context.builder());
        } else {
            static_assert(std::invocable<decltype(Main), ModuleContext const&>, "module main must be void(GraphBuilder&)");
            static_assert(std::same_as<std::invoke_result_t<decltype(Main), ModuleContext const&>, void>, "module main must return void");
            Main(context);
        }
    }
    template<auto Main>
    char const* generated_module_build_v1(ModuleContext const& context) noexcept {
        static thread_local char last_error[2048];
        try { last_error[0] = '\0'; invoke_generated_main<Main>(context); return nullptr; }
        catch (std::exception const& e) { std::snprintf(last_error, sizeof(last_error), "%s", e.what()); return last_error; }
        catch (...) { std::snprintf(last_error, sizeof(last_error), "%s", "non-std exception"); return last_error; }
    }
}
