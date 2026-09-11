#pragma once

#include <intravenous/node/code_key.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace iv {
namespace details {
struct BuilderSession;
}
inline constexpr std::uint32_t IV_MODULE_ABI_VERSION = 13;

struct ModuleDataView {
    void const* data = nullptr;
    std::size_t size = 0;
};

struct ModuleNodeConfigRecord {
    void const* data = nullptr;
    std::size_t size = 0;
    std::size_t alignment = 1;
};

struct NodeConfigPointerFieldData {
    NodeCodeKey code_key{};
    std::size_t byte_offset = 0;
};

struct RetainedGlobalData {
    void const* address = nullptr;
    std::size_t size = 0;
    std::size_t ordinal = 0;
};

static_assert(std::is_standard_layout_v<NodeConfigPointerFieldData>);
static_assert(std::is_trivially_copyable_v<NodeConfigPointerFieldData>);
static_assert(std::is_standard_layout_v<RetainedGlobalData>);
static_assert(std::is_trivially_copyable_v<RetainedGlobalData>);

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
using iv_module_node_types_fn = iv::ModuleDataView (*)();
using iv_source_node_types_fn = iv::ModuleDataView (*)();
using iv_source_registrations_fn = iv::ModuleDataView (*)();
using iv_source_node_config_pointer_fields_fn = iv::ModuleDataView (*)();
using iv_source_retained_globals_fn = iv::ModuleDataView (*)();
}

#if defined(_WIN32)
#define IV_MODULE_EXPORT __declspec(dllexport)
#else
#define IV_MODULE_EXPORT __attribute__((visibility("default")))
#endif
