#pragma once

#include <intravenous/graph/builder.h>

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <type_traits>

extern "C" {
    [[maybe_unused]] static constexpr uint32_t IV_MODULE_ABI_VERSION_V2 = 2;
    using iv_module_build_fn_v2 = char const* (*)(iv::GraphBuilder&);
    struct iv_module_descriptor_v2 {
        uint32_t abi_version;
        char const* id;
        iv_module_build_fn_v2 build;
    };
    using iv_get_module_descriptor_fn_v2 = iv_module_descriptor_v2 const* (*)();
}

#if defined(_WIN32)
#define IV_MODULE_EXPORT __declspec(dllexport)
#else
#define IV_MODULE_EXPORT __attribute__((visibility("default")))
#endif

namespace iv::details {
    template<auto Main>
    char const* generated_module_build_v2(GraphBuilder& builder) noexcept
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
