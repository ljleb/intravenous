#pragma once

#include <intravenous/graph/authored_graph_view.hpp>

#include <cstdint>

namespace iv {
inline constexpr std::uint32_t IV_MODULE_ABI_VERSION = 3;
}

// A module exports only the frozen authored graph. The host owns lowering,
// compilation, and execution-plan lifetime.
extern "C" {
using iv_module_authored_graph_fn = iv::AuthoredGraphView (*)();
using iv_module_abi_version_fn = std::uint32_t (*)();
}

#if defined(_WIN32)
#define IV_MODULE_EXPORT __declspec(dllexport)
#else
#define IV_MODULE_EXPORT __attribute__((visibility("default")))
#endif
