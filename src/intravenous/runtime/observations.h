#pragma once

#include <intravenous/runtime/observation_events.h>
#include <intravenous/runtime/socket_rpc_requests.h>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace iv {
class Observations {
    using Clock = std::chrono::steady_clock;
    struct Scope {
        std::string client;
        std::string scope;
        std::vector<ObservationInterest> interests{};
        Clock::time_point expires{};
    };
    struct WindowCapture {
        ObservationInterest interest{};
        std::shared_ptr<ObservationWindowQuery> query{};
        std::shared_ptr<nlohmann::ordered_json const> payload{};
        std::shared_ptr<ObservationWindowBody const> body{};
        std::shared_ptr<ObservationWindowQuery> payload_query{};
        Clock::time_point retry_after{};
        bool submitted = false;
    };
    std::vector<WindowCapture> windows_{};
    std::optional<std::uint64_t> configured_generation_{};
    std::uint64_t execution_data_version_ = 0;
    std::mutex mutex_;
    std::vector<Scope> scopes_;
    ObservationRequirements accepted_{};
    std::uint64_t last_scheduling_us_ = 0;
    bool requirements_dirty_ = false;
    bool compilation_failed_ = false;
    std::uint64_t next_id_ = 1;
    std::uint64_t next_window_body_id_ = 1;
    std::uint64_t last_activation_ = 0;
    std::optional<LiveObservationFrame> latest_{};
    std::unordered_map<std::string, std::string> last_delivered_;
    Clock::time_point next_collection_{};
    bool reconcile_locked();
public:
    void handle_protocol(ObservationProtocolRequest& request);
    void handle_disconnect(SocketRpcClientDisconnectedRequest& request);
    // Called by the existing runtime service on the control path. It is a
    // separate event cause, with no audio-thread callbacks into app modules.
    void collect(Clock::time_point now = Clock::now());
};
} // namespace iv
