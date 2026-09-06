#pragma once

#include <cstdint>

namespace iv {
inline constexpr std::uint32_t IV_MODULE_ABI_VERSION = 4;
}

extern "C" {
using iv_module_abi_version_fn = std::uint32_t (*)();
}

#if defined(_WIN32)
#define IV_MODULE_EXPORT __declspec(dllexport)
#else
#define IV_MODULE_EXPORT __attribute__((visibility("default")))
#endif
