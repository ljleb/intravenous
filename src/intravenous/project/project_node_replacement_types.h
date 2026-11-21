#pragma once

#include <intravenous/node/address.h>

#include <filesystem>
#include <optional>
#include <string>

namespace iv {
// Canonical durable successor for one semantic node. The JSON value is the
// complete typed argument list supplied by the replacement handler. It is not
// a patch over the enclosing module's authored construction call.
struct ProjectNodeReplacement {
    NodeAddress address{};
    std::string definition_id{};
    std::optional<std::filesystem::path> package_root{};
    std::string configuration_arguments_json{"[]"};

    bool operator==(ProjectNodeReplacement const&) const = default;
};
} // namespace iv
