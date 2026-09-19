#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace iv {
    // Stable source-derived identity for one C++ type definition. The nominal
    // ID identifies the declared type (Clang USR); the fingerprint identifies
    // the exact definition accepted for this package generation. display_name
    // is diagnostic-only and deliberately does not participate in equality.
    struct TypeDefinitionIdentity {
        std::string nominal_id {};
        std::string definition_fingerprint {};
        std::string display_name {};

        [[nodiscard]] bool valid() const noexcept
        {
            return !nominal_id.empty() && !definition_fingerprint.empty();
        }

        bool operator==(TypeDefinitionIdentity const& other) const noexcept
        {
            return nominal_id == other.nominal_id
                && definition_fingerprint == other.definition_fingerprint;
        }
    };

    struct NodeStateFieldStructure {
        std::string name {};
        std::string type_name {};
        size_t bit_offset = 0;
        size_t size_bits = 0;
        size_t alignment_bits = 0;
        std::optional<size_t> bit_width {};

        bool operator==(NodeStateFieldStructure const&) const = default;
    };

    struct NodeStateStructure {
        TypeDefinitionIdentity type_identity {};
        size_t size_bits = 0;
        size_t alignment_bits = 0;
        std::vector<NodeStateFieldStructure> fields {};

        bool operator==(NodeStateStructure const&) const = default;
    };

    // Source-produced metadata for both persistent node-state domains. Keeping
    // these together prevents declaration/migration paths from accidentally
    // treating CompiledState as a weaker ABI than State.
    struct NodeStateStructures {
        std::optional<NodeStateStructure> state {};
        std::optional<NodeStateStructure> compiled_state {};

        bool operator==(NodeStateStructures const&) const = default;
    };
}
