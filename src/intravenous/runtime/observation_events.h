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
    bool prefetch = false;
    bool operator==(ObservationInterest const&) const = default;
};
struct ObservationProtocolRequest {
    enum class Kind { describe, reconcile } kind = Kind::describe;
    std::string client_id; // Bound by the server, never parsed from the payload.
    std::string scope_id;
    std::vector<NodeAddress> nodes{};
    std::vector<ObservationInterest> interests{};
    std::vector<ObservationInterest> predicted_interests{};
    std::shared_ptr<nlohmann::ordered_json const> result{};
    std::string error{};
    bool handled = false;
};
struct DescribeObservationsRequest : ObservationProtocolRequest {};
struct ReconcileObservationsRequest : ObservationProtocolRequest {};
struct ObservationGraphRequest {
    std::optional<ObservationRequirements> requirements{};
    bool prediction_changed = false;
    std::optional<ObservationRequirements> prediction{};
    std::vector<NodeAddress> describe_nodes{};
    struct WindowSource {
        ObservationSource const& source;
        std::string_view operation;
        ObservationWindow window{};
        bool prefetch = false;
        std::shared_ptr<ObservationWindowQuery> query{};

        // Submitted queries belong to the executor until it completes them.
        // Resolve a fresh query whenever the checkpoint changes.
        bool select_execution(std::uint64_t configured, std::optional<std::uint64_t> execution)
        {
            if (query && query->configured_generation == configured && query->execution_generation == execution)
                return false;
            query = std::make_shared<ObservationWindowQuery>();
            query->window = window;
            query->configured_generation = configured;
            query->execution_generation = execution;
            query->prefetch.store(prefetch, std::memory_order_relaxed);
            return true;
        }
    };
    std::vector<WindowSource> windows{};
    std::shared_ptr<nlohmann::ordered_json const> capabilities{};
    std::string error{};
    std::optional<std::uint64_t> configured_generation{};
    std::optional<std::uint64_t> window_configured_generation{};
    std::optional<std::uint64_t> window_execution_generation{};
    std::optional<std::size_t> window_sample_rate{};
    std::uint64_t scheduling_us = 0;
    bool handled = false;
};

struct ObservationWindowBody;
struct ObservationDelivery {
    std::string client_id;
    std::shared_ptr<nlohmann::ordered_json const> payload{};
    std::vector<std::shared_ptr<ObservationWindowBody const>> window_bodies{};
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
