#pragma once

#include <intravenous/basic_nodes/weak_type_erased.h>
#include <intravenous/graph/builder.h>

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <type_traits>

extern "C" {
    [[maybe_unused]] static constexpr uint32_t IV_MODULE_ABI_VERSION_V3 = 3;
    using iv_module_build_fn_v3 = char const* (*)(iv::GraphBuilder&);
    using iv_module_create_fn_v3 = iv::WeakTypeErasedNode (*)();
    struct iv_module_descriptor_v3 {
        uint32_t abi_version;
        char const* id;
        iv_module_build_fn_v3 build;
        iv_module_create_fn_v3 create;
    };
    using iv_get_module_descriptor_fn_v3 = iv_module_descriptor_v3 const* (*)();
}

#if defined(_WIN32)
#define IV_MODULE_EXPORT __declspec(dllexport)
#else
#define IV_MODULE_EXPORT __attribute__((visibility("default")))
#endif

namespace iv::details {
    template<auto Main>
    consteval Graph generated_module_graph_v3()
    {
        static_assert(std::invocable<decltype(Main), GraphBuilder&>,
            "iv_module.json main must name void(GraphBuilder&)");
        static_assert(std::same_as<std::invoke_result_t<decltype(Main), GraphBuilder&>, void>,
            "iv_module.json main must name void(GraphBuilder&)");

        GraphBuilder builder;
        Main(builder);
        return builder.build_execution_root_node().graph;
    }

    template<auto Main>
    WeakTypeErasedNode generated_module_create_v3() noexcept
    {
        static constexpr Graph graph = generated_module_graph_v3<Main>();
        return WeakTypeErasedNode(graph);
    }

    // Introspection still uses the authoring builder while its persistent
    // metadata representation is migrated. Execution never consumes this
    // builder: create() above returns the DSO-static compiled root.
    template<auto Main>
    char const* generated_module_build_v3(GraphBuilder& builder) noexcept
    {
        static_assert(std::invocable<decltype(Main), GraphBuilder&>,
            "iv_module.json main must name void(GraphBuilder&)");
        static_assert(std::same_as<std::invoke_result_t<decltype(Main), GraphBuilder&>, void>,
            "iv_module.json main must name void(GraphBuilder&)");

        static thread_local char last_error[2048];
        try {
            last_error[0] = '\0';
            Main(builder);
            return nullptr;
        } catch (std::exception const& e) {
            std::snprintf(last_error, sizeof(last_error), "%s", e.what());
            return last_error;
        } catch (...) {
            std::snprintf(last_error, sizeof(last_error), "%s", "non-std exception");
            return last_error;
        }
    }
}
