#pragma once

#include <intravenous/node/code_key.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace iv {
namespace details {
struct BuilderSession;
}
inline constexpr std::uint32_t IV_MODULE_ABI_VERSION = 12;

struct ModuleDataView {
    void const* data = nullptr;
    std::size_t size = 0;
};

struct ModuleNodeConfigRecord {
    void const* data = nullptr;
    std::size_t size = 0;
    std::size_t alignment = 1;
};

// A stable source-level association between a registered primitive node ID and
// the build-local compiler key used by the artifact's LLVM table.  The key is
// intentionally not persistent; the ID is the registry identity.
struct SourceNodeTypeData {
    ModuleDataView id{};
    NodeCodeKey code_key{};
    ModuleDataView authored_graph{};
    ModuleDataView node_configs{};
};

static_assert(std::is_standard_layout_v<SourceNodeTypeData>);
static_assert(std::is_trivially_copyable_v<SourceNodeTypeData>);

}

extern "C" {
using iv_module_abi_version_fn = std::uint32_t (*)();
using iv_source_module_count_fn = std::size_t (*)();
using iv_source_module_id_fn = iv::ModuleDataView (*)(std::size_t);
using iv_source_module_authored_graph_fn = iv::ModuleDataView (*)(std::size_t);
using iv_source_module_node_configs_fn = iv::ModuleDataView (*)(std::size_t);
using iv_module_node_types_fn = iv::ModuleDataView (*)();
using iv_source_node_types_fn = iv::ModuleDataView (*)();
using iv_source_registered_module_count_fn = std::size_t (*)();
using iv_source_registered_module_id_fn = iv::ModuleDataView (*)(std::size_t);
using iv_source_build_registered_module_fn = void (*)(
    std::size_t, iv::details::BuilderSession*);
}

#if defined(_WIN32)
#define IV_MODULE_EXPORT __declspec(dllexport)
#else
#define IV_MODULE_EXPORT __attribute__((visibility("default")))
#endif
