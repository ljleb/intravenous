#pragma once

#include <string>

namespace iv {

// Stable registry provenance for a concrete primitive node realization.
// NodeCodeKey remains the build-local compiler join; this identity tells later
// whole-project compilation which registered primitive/provider produced the
// configured node.
struct RegisteredNodeTypeIdentity {
    std::string node_type_id{};
    std::string provider_package_root{};

    constexpr bool operator==(RegisteredNodeTypeIdentity const&) const = default;
};

} // namespace iv
