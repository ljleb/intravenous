#pragma once

#include <intravenous/node/code_key.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace iv {
inline constexpr std::uint32_t IV_PACKAGE_ABI_VERSION = 14;

struct ModuleDataView {
    void const* data = nullptr;
    std::size_t size = 0;
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

struct NodeStateFieldData {
    ModuleDataView name{};
    ModuleDataView type_name{};
    std::size_t bit_offset = 0;
    std::size_t size_bits = 0;
    std::size_t alignment_bits = 0;
    std::size_t bit_width = 0;
    std::uint8_t has_bit_width = 0;
};

struct NodeStateStructureData {
    NodeCodeKey code_key{};
    std::size_t size_bits = 0;
    std::size_t alignment_bits = 0;
    ModuleDataView fields{};
};

static_assert(std::is_standard_layout_v<ModuleDataView>);
static_assert(std::is_trivially_copyable_v<ModuleDataView>);
static_assert(std::is_standard_layout_v<NodeConfigPointerFieldData>);
static_assert(std::is_trivially_copyable_v<NodeConfigPointerFieldData>);
static_assert(std::is_standard_layout_v<RetainedGlobalData>);
static_assert(std::is_trivially_copyable_v<RetainedGlobalData>);
static_assert(std::is_standard_layout_v<NodeStateFieldData>);
static_assert(std::is_trivially_copyable_v<NodeStateFieldData>);
static_assert(std::is_standard_layout_v<NodeStateStructureData>);
static_assert(std::is_trivially_copyable_v<NodeStateStructureData>);
}

extern "C" {
using iv_package_abi_version_fn = std::uint32_t (*)();
using iv_package_registrations_fn = iv::ModuleDataView (*)();
using iv_package_node_config_pointer_fields_fn = iv::ModuleDataView (*)();
using iv_package_retained_globals_fn = iv::ModuleDataView (*)();
using iv_package_node_state_structures_fn = iv::ModuleDataView (*)();
}
