#pragma once

#include <filesystem>
#include <vector>

namespace iv {
// Immutable compiler inputs for one finalized IV package generation. The
// bitcode artifact is O0 package LLVM; dynamic_libraries contains the native
// libraries that must be available when that LLVM is linked into a JIT domain.
struct PackageCompilerArtifact {
    std::filesystem::path bitcode_path{};
    std::vector<std::filesystem::path> dynamic_libraries{};
};
} // namespace iv
