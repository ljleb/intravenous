#pragma once

#include <intravenous/node/code_key.h>
#include <intravenous/node/compiler_record.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace iv {
struct NodeAddress;
struct NodeMessageContextOperations;
struct ModuleMessageLocalRefs;
namespace details {
struct ConfigurationTypeIdentity;
}
// The loader rejects a package whose iv_package_abi_version() differs from this
// value. It stays 0 and is never raised: a package build signature already
// contains this header's text and the identity of the DSL precompiled header,
// so a changed host header rebuilds every package.
inline constexpr std::uint32_t IV_PACKAGE_ABI_VERSION = 0;

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
    std::size_t index = 0;
};

// Node code is the finalizer's native object beside the finalized package
// LLVM: every node record's runtime/compiler callbacks and a copy of every
// retained global, optimized at O3 and compiled for the host CPU once per
// package build. Project code calls these callbacks; configuration keeps using
// the package's own code.
//
// One record per NodeCompilerRecord, sorted by code_key. operations has the
// same optional callbacks as that record, except declare_node, which is
// configuration-only and always null here.
struct NodeCodeRecordData {
    NodeCodeKey code_key{};
    details::NodeCompilerOperations operations{};
};

// The node-code copy of one retained global, indexed like RetainedGlobalData.
// A null address marks a global that cannot be node-code data (not constant,
// thread-local, or no longer a plain global after package optimization); only a
// configuration relocation that uses it fails.
struct NodeCodeRetainedGlobalData {
    void const* address = nullptr;
    std::size_t size = 0;
    std::size_t alignment = 1;
};

struct NodeMessageHandlerData {
    NodeCodeKey code_key{};
    details::ConfigurationTypeIdentity const* event_type = nullptr;
    void (*invoke)(
        void const*,
        NodeMessageContextOperations const&,
        NodeAddress const&,
        ModuleMessageLocalRefs const*,
        void const*) = nullptr;
};

struct ModuleMessageHandlerData {
    ModuleDataView definition_id{};
    details::ConfigurationTypeIdentity const* event_type = nullptr;
    void (*invoke)(
        void const*,
        NodeMessageContextOperations const&,
        NodeAddress const&,
        ModuleMessageLocalRefs const*,
        void const*) = nullptr;
};

struct NodeStateFieldData {
    ModuleDataView name{};
    ModuleDataView nominal_id{};
    ModuleDataView type_name{};
    std::size_t bit_offset = 0;
    std::size_t size_bits = 0;
    std::size_t alignment_bits = 0;
    std::size_t bit_width = 0;
    std::uint8_t has_bit_width = 0;
};

struct NodeStateStructureData {
    NodeCodeKey code_key{};
    ModuleDataView nominal_id{};
    ModuleDataView definition_fingerprint{};
    ModuleDataView display_name{};
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
static_assert(std::is_standard_layout_v<NodeCodeRecordData>);
static_assert(std::is_trivially_copyable_v<NodeCodeRecordData>);
static_assert(std::is_standard_layout_v<NodeCodeRetainedGlobalData>);
static_assert(std::is_trivially_copyable_v<NodeCodeRetainedGlobalData>);
static_assert(std::is_standard_layout_v<NodeMessageHandlerData>);
static_assert(std::is_trivially_copyable_v<NodeMessageHandlerData>);
static_assert(std::is_standard_layout_v<ModuleMessageHandlerData>);
static_assert(std::is_trivially_copyable_v<ModuleMessageHandlerData>);
static_assert(std::is_standard_layout_v<NodeStateFieldData>);
static_assert(std::is_trivially_copyable_v<NodeStateFieldData>);
static_assert(std::is_standard_layout_v<NodeStateStructureData>);
static_assert(std::is_trivially_copyable_v<NodeStateStructureData>);
}

extern "C" {
using iv_package_abi_version_fn = std::uint32_t (*)();
using iv_package_definitions_fn = iv::ModuleDataView (*)();
using iv_package_node_config_pointer_fields_fn = iv::ModuleDataView (*)();
using iv_package_retained_globals_fn = iv::ModuleDataView (*)();
using iv_package_node_message_handlers_fn = iv::ModuleDataView (*)();
using iv_package_module_message_handlers_fn = iv::ModuleDataView (*)();
using iv_package_node_state_structures_fn = iv::ModuleDataView (*)();
using iv_package_node_background_state_structures_fn = iv::ModuleDataView (*)();
// The two exports of the node-code object.
using iv_package_node_code_records_fn = iv::ModuleDataView (*)();
using iv_package_node_code_retained_globals_fn = iv::ModuleDataView (*)();
}
