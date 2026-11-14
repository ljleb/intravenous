#pragma once

#include <filesystem>
#include <vector>

namespace iv {
// Immutable compiler inputs for one finalized IV package generation. The
// bitcode artifact is package LLVM after iv_package_finalize's pre-link
// simplification; dynamic_libraries contains the native libraries that must
// be available when that LLVM is linked into a JIT domain. The node-code
// object `<bitcode_path>.node-code.o` sits beside it; ModuleLoader links it
// with the package and PackageRevision carries its tables.
struct PackageCompilerArtifact {
    std::filesystem::path bitcode_path{};
    // Language-neutral semantic event schema emitted beside the finalized
    // package. Frontend-specific binding generators consume this artifact;
    // the backend does not emit TypeScript or another frontend language.
    std::filesystem::path node_event_protocol_path{};
    std::vector<std::filesystem::path> dynamic_libraries{};
};
} // namespace iv
