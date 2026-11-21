#pragma once

#include <intravenous/node/address.h>
#include <string>

namespace iv {
struct NodeMessageGestureId {
    std::string client_id{};
    std::string interaction_id{};
    bool operator==(NodeMessageGestureId const&) const = default;
};

struct NodeMessageGestureRequest {
    NodeMessageGestureId id{};
    NodeAddress target{};
    bool begin = false;
};
struct BeginNodeMessageGestureRequest : NodeMessageGestureRequest {};
struct EndNodeMessageGestureRequest : NodeMessageGestureRequest {};
} // namespace iv
