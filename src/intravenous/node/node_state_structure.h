#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace iv {
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
        size_t size_bits = 0;
        size_t alignment_bits = 0;
        std::vector<NodeStateFieldStructure> fields {};

        bool operator==(NodeStateStructure const&) const = default;
    };
}
