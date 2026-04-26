#pragma once

#include "ports.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
    enum class LaneDomain : std::uint8_t {
        compiled,
        realtime,
    };

    struct LaneId {
        std::uint64_t value = 0;

        constexpr explicit operator bool() const noexcept
        {
            return value != 0;
        }

        bool operator==(LaneId const&) const = default;
    };

    struct LaneIdHash {
        size_t operator()(LaneId id) const noexcept
        {
            return std::hash<std::uint64_t> {}(id.value);
        }
    };

    class LaneIdAllocator {
        std::atomic<std::uint64_t> _next { 1 };

    public:
        LaneId next()
        {
            return LaneId { _next.fetch_add(1, std::memory_order_relaxed) };
        }
    };

    struct LaneEndpointId {
        LaneId lane {};

        bool operator==(LaneEndpointId const&) const = default;
    };

    struct LaneInputId {
        LaneId lane {};
        PortKind kind = PortKind::sample;
        size_t ordinal = 0;

        bool operator==(LaneInputId const&) const = default;
    };

    struct LaneConnection {
        LaneEndpointId source {};
        LaneInputId target {};

        bool operator==(LaneConnection const&) const = default;
    };

    struct GraphInputLaneTarget {
        std::string logical_node_id {};
        std::optional<size_t> concrete_member_ordinal {};
        PortKind port_kind = PortKind::sample;
        size_t port_ordinal = 0;
        std::string port_name {};
        std::string port_type {};

        bool operator==(GraphInputLaneTarget const&) const = default;
    };

    struct GraphInputLane {
        LaneId id {};
        GraphInputLaneTarget target {};
    };

    struct DanglingLaneConnection {
        LaneConnection connection {};
        std::optional<GraphInputLaneTarget> missing_target {};
    };

    class GraphInputLaneRegistry {
        LaneIdAllocator _ids;
        std::unordered_map<std::string, LaneId> _lane_ids_by_key;
        std::unordered_map<LaneId, GraphInputLaneTarget, LaneIdHash> _targets_by_lane_id;
        std::unordered_map<LaneId, GraphInputLaneTarget, LaneIdHash> _last_targets_by_lane_id;

        static std::string target_key(GraphInputLaneTarget const& target)
        {
            std::string key = target.logical_node_id;
            key += "\x1fmember:";
            if (target.concrete_member_ordinal.has_value()) {
                key += std::to_string(*target.concrete_member_ordinal);
            } else {
                key += "logical";
            }
            key += "\x1fkind:";
            key += target.port_kind == PortKind::sample ? "sample" : "event";
            key += "\x1fordinal:";
            key += std::to_string(target.port_ordinal);
            return key;
        }

    public:
        std::vector<GraphInputLane> reconcile(std::vector<GraphInputLaneTarget> const& targets)
        {
            std::unordered_set<std::string> live_keys;
            std::vector<GraphInputLane> lanes;
            lanes.reserve(targets.size());

            for (auto const& target : targets) {
                auto const key = target_key(target);
                live_keys.insert(key);

                auto [it, inserted] = _lane_ids_by_key.emplace(key, LaneId {});
                if (inserted || !it->second) {
                    it->second = _ids.next();
                }
                _targets_by_lane_id[it->second] = target;
                _last_targets_by_lane_id[it->second] = target;
                lanes.push_back(GraphInputLane {
                    .id = it->second,
                    .target = target,
                });
            }

            for (auto it = _lane_ids_by_key.begin(); it != _lane_ids_by_key.end();) {
                if (live_keys.contains(it->first)) {
                    ++it;
                    continue;
                }
                _targets_by_lane_id.erase(it->second);
                it = _lane_ids_by_key.erase(it);
            }

            return lanes;
        }

        std::optional<GraphInputLaneTarget> target_for(LaneId lane_id) const
        {
            auto const it = _targets_by_lane_id.find(lane_id);
            if (it == _targets_by_lane_id.end()) {
                return std::nullopt;
            }
            return it->second;
        }

        std::vector<DanglingLaneConnection> dangling_connections(
            std::vector<LaneConnection> const& connections
        ) const
        {
            std::vector<DanglingLaneConnection> dangling;
            for (auto const& connection : connections) {
                if (_targets_by_lane_id.contains(connection.target.lane)) {
                    continue;
                }
                auto const missing_target = _last_targets_by_lane_id.find(connection.target.lane);
                dangling.push_back(DanglingLaneConnection {
                    .connection = connection,
                    .missing_target = missing_target == _last_targets_by_lane_id.end()
                        ? std::nullopt
                        : std::optional<GraphInputLaneTarget>(missing_target->second),
                });
            }
            return dangling;
        }
    };
}
