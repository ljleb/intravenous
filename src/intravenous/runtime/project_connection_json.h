#pragma once

#include <intravenous/runtime/project_connection_types.h>

#include <nlohmann/json_fwd.hpp>

namespace iv {
enum class ProjectConnectionJsonNames {
    project_file,
    socket_rpc,
};

[[nodiscard]] nlohmann::ordered_json project_connection_json(
    ProjectConnection const& connection,
    ProjectConnectionJsonNames names);

[[nodiscard]] ProjectConnection parse_project_connection_json(
    nlohmann::ordered_json const& json,
    ProjectConnectionJsonNames names);
} // namespace iv
