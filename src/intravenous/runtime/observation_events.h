#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/observation_types.h>

#include <memory>
#include <nlohmann/json_fwd.hpp>

namespace iv {
struct ObservationInterest {
    std::string id;
    ObservationSource source{};
    std::string operation;
    std::optional<ObservationWindow> window{};
};
struct ObservationProtocolRequest {
    enum class Kind { describe, reconcile } kind = Kind::describe;
    std::string client_id; // Bound by the server, never parsed from the payload.
    std::string scope_id;
    std::vector<NodeAddress> nodes{};
    std::vector<ObservationInterest> interests{};
    std::shared_ptr<nlohmann::ordered_json const> result{};
    std::string error{};
    bool handled = false;
};
struct DescribeObservationsRequest : ObservationProtocolRequest {};
struct ReconcileObservationsRequest : ObservationProtocolRequest {};
struct ObservationGraphRequest {
    std::optional<ObservationRequirements> requirements{};
    std::vector<NodeAddress> describe_nodes{};
    struct WindowSource {
        ObservationSource source{};
        std::string operation{};
        std::shared_ptr<ObservationWindowQuery> query{};
    };
    std::vector<WindowSource> windows{};
    std::shared_ptr<nlohmann::ordered_json const> capabilities{};
    std::string error{};
    std::optional<std::uint64_t> configured_generation{};
    std::uint64_t scheduling_us = 0;
    bool handled = false;
};

struct ObservationDelivery {
    std::string client_id;
    std::shared_ptr<nlohmann::ordered_json const> payload{};
};
using SocketRpcObservationEvent = void (*)(ObservationProtocolRequest&);
using ObservationsProjectGraphEvent = void (*)(ObservationGraphRequest&);
using ObservationsGraphExecutorEvent = void (*)(ObservationCollectRequest&);
using ObservationsSocketRpcEvent = void (*)(std::span<ObservationDelivery const>);
IV_DECLARE_LINKER_EVENT(SocketRpcObservationEvent, iv_socket_rpc_observation_event);
IV_DECLARE_LINKER_EVENT(ObservationsProjectGraphEvent, iv_observations_project_graph_event);
IV_DECLARE_LINKER_EVENT(ObservationsGraphExecutorEvent, iv_observations_graph_executor_event);
IV_DECLARE_LINKER_EVENT(ObservationsSocketRpcEvent, iv_observations_socket_rpc_event);
} // namespace iv
