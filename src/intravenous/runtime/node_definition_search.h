#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <optional>
#include <string_view>

namespace iv {
struct NodeDefinitionsSnapshot;

[[nodiscard]] nlohmann::ordered_json search_node_definitions(
    NodeDefinitionsSnapshot const&, std::string_view source, std::optional<std::size_t> cursor);
}
