#pragma once

#include <cstddef>
#include <cstdint>

namespace iv {
inline constexpr std::uint32_t IV_MODULE_ABI_VERSION = 8;

struct ModuleDataView {
    void const* data = nullptr;
    std::size_t size = 0;
};

struct ModuleNodeConfigRecord {
    void const* data = nullptr;
    std::size_t size = 0;
    std::size_t alignment = 1;
};

struct ModuleNodeConfigStringRelocationRecord {
    std::size_t config_ordinal = 0;
    std::size_t byte_offset = 0;
    char const* string_data = nullptr;
    std::size_t string_size = 0;
};
}

extern "C" {
using iv_module_abi_version_fn = std::uint32_t (*)();
using iv_module_authored_graph_fn = iv::ModuleDataView (*)();
using iv_module_node_configs_fn = iv::ModuleDataView (*)();
using iv_module_node_config_string_relocations_fn = iv::ModuleDataView (*)();
using iv_module_node_types_fn = iv::ModuleDataView (*)();
}

#if defined(_WIN32)
#define IV_MODULE_EXPORT __declspec(dllexport)
#else
#define IV_MODULE_EXPORT __attribute__((visibility("default")))
#endif
