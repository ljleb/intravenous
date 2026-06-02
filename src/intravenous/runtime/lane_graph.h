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
        void observe(LaneId id)
        {
            auto current = _next.load(std::memory_order_relaxed);
            auto desired = id.value + 1;
            while (current < desired &&
                   !_next.compare_exchange_weak(
                       current,
                       desired,
                       std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
            }
        }

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

    struct GraphInputPortDescriptor {
        std::string logical_node_id {};
        std::optional<size_t> concrete_member_ordinal {};
        PortKind port_kind = PortKind::sample;
        size_t port_ordinal = 0;
        std::string port_name {};
        std::string port_type {};

        bool operator==(GraphInputPortDescriptor const&) const = default;
    };

    struct GraphInputLane {
        LaneId id {};
        GraphInputPortDescriptor port {};
    };

    struct DanglingLaneConnection {
        LaneConnection connection {};
        std::optional<GraphInputPortDescriptor> missing_port {};
    };

    class GraphInputLaneRegistry {
        LaneIdAllocator _ids;
        std::unordered_map<std::string, LaneId> _lane_ids_by_key;
        std::unordered_map<LaneId, GraphInputPortDescriptor, LaneIdHash> _ports_by_lane_id;
        std::unordered_map<LaneId, GraphInputPortDescriptor, LaneIdHash> _last_ports_by_lane_id;

        static std::string port_descriptor_key(GraphInputPortDescriptor const& port)
        {
            std::string key = port.logical_node_id;
            key += "\x1fmember:";
            if (port.concrete_member_ordinal.has_value()) {
                key += std::to_string(*port.concrete_member_ordinal);
            } else {
                key += "logical";
            }
            key += "\x1fkind:";
            key += port.port_kind == PortKind::sample ? "sample" : "event";
            key += "\x1fordinal:";
            key += std::to_string(port.port_ordinal);
            return key;
        }

    public:
        std::vector<GraphInputLane> reconcile(std::vector<GraphInputPortDescriptor> const& ports)
        {
            std::unordered_set<std::string> live_keys;
            std::vector<GraphInputLane> lanes;
            lanes.reserve(ports.size());

            for (auto const& port : ports) {
                auto const key = port_descriptor_key(port);
                live_keys.insert(key);

                auto [it, inserted] = _lane_ids_by_key.emplace(key, LaneId {});
                if (inserted || !it->second) {
                    it->second = _ids.next();
                }
                _ports_by_lane_id[it->second] = port;
                _last_ports_by_lane_id[it->second] = port;
                lanes.push_back(GraphInputLane {
                    .id = it->second,
                    .port = port,
                });
            }

            for (auto it = _lane_ids_by_key.begin(); it != _lane_ids_by_key.end();) {
                if (live_keys.contains(it->first)) {
                    ++it;
                    continue;
                }
                _ports_by_lane_id.erase(it->second);
                it = _lane_ids_by_key.erase(it);
            }

            return lanes;
        }

        std::optional<GraphInputPortDescriptor> port_descriptor_for(LaneId lane_id) const
        {
            auto const it = _ports_by_lane_id.find(lane_id);
            if (it == _ports_by_lane_id.end()) {
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
                if (_ports_by_lane_id.contains(connection.target.lane)) {
                    continue;
                }
                auto const missing_port = _last_ports_by_lane_id.find(connection.target.lane);
                dangling.push_back(DanglingLaneConnection {
                    .connection = connection,
                    .missing_port = missing_port == _last_ports_by_lane_id.end()
                        ? std::nullopt
                        : std::optional<GraphInputPortDescriptor>(missing_port->second),
                });
            }
            return dangling;
        }
    };
}
