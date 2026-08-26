#pragma once

#include <intravenous/basic_nodes/weak_type_erased.h>
#include <intravenous/graph/static_metadata.hpp>

#include <cstdint>

namespace iv {
inline constexpr std::uint32_t IV_MODULE_ABI_VERSION = 2;
}

// The runtime loader and a compiled module communicate only through these
// immutable products. Keep this header independent from graph authoring so
// ordinary runtime translation units never need to parse GraphBuilder.
extern "C" {
using iv_module_graph_fn = iv::WeakTypeErasedNode (*)();
using iv_module_metadata_fn = iv::StaticGraphIntrospectionMetadata (*)();
using iv_module_abi_version_fn = std::uint32_t (*)();
}

#if defined(_WIN32)
#define IV_MODULE_EXPORT __declspec(dllexport)
#else
#define IV_MODULE_EXPORT __attribute__((visibility("default")))
#endif
