#pragma once

#include <intravenous/runtime/observation_events.h>
#include <intravenous/runtime/socket_rpc_requests.h>
#include <chrono>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>

namespace iv {
class Observations {
    using Clock = std::chrono::steady_clock;
    struct WindowCapture;
    struct InterestCapture {
        WindowCapture* window = nullptr;
        std::uint64_t probe_id = 0;
    };
    struct Scope {
        std::string client;
        std::string scope;
        std::vector<ObservationInterest> interests{};
        std::vector<ObservationInterest> predicted_interests{};
        std::uint64_t prediction_sequence = 0;
        std::vector<InterestCapture> captures{};
        Clock::time_point expires{};
    };
    struct WindowCapture {
        ObservationInterest interest{};
        std::size_t subscribers = 0;
        std::shared_ptr<ObservationWindowQuery> query{};
        std::shared_ptr<nlohmann::ordered_json const> payload{};
        std::shared_ptr<ObservationWindowBody const> body{};
        std::shared_ptr<ObservationWindowQuery> payload_query{};
        Clock::time_point requested_at{};
        bool submitted = false;
    };
    using WindowKey = std::tuple<ObservationSource, std::string, std::uint64_t, std::uint64_t,
        std::size_t, std::optional<std::uint8_t>>;
    std::map<WindowKey, WindowCapture> windows_{};
    std::vector<WindowCapture*> window_order_{};
    bool subscriptions_changed_ = false;
    std::optional<std::uint64_t> window_configured_generation_{};
    std::optional<std::uint64_t> window_execution_generation_{};
    std::uint64_t execution_data_version_ = 0;
    std::mutex mutex_;
    std::vector<Scope> scopes_;
    using ProbeKey = std::pair<ObservationSource, std::string>;
    ObservationRequirements accepted_{};
    std::map<ProbeKey, std::uint64_t> active_probe_ids_{};
    std::optional<ObservationRequirements> predicted_{};
    std::uint64_t next_prediction_sequence_ = 0;
    std::uint64_t last_scheduling_us_ = 0;
    bool requirements_dirty_ = false;
    bool compilation_failed_ = false;
    std::uint64_t next_id_ = 1;
    std::uint64_t next_window_body_id_ = 1;
    std::uint64_t last_activation_ = 0;
    std::optional<LiveObservationFrame> latest_{};
    std::unordered_map<std::uint64_t, std::size_t> live_capture_indices_{};
    std::unordered_map<std::string, std::string> last_delivered_;
    Clock::time_point next_collection_{};
    bool reconcile_locked();
    void update_subscriptions_locked();
public:
    void handle_protocol(ObservationProtocolRequest& request);
    void handle_disconnect(SocketRpcClientDisconnectedRequest& request);
    // Called by the existing runtime service on the control path. It is a
    // separate event cause, with no audio-thread callbacks into app modules.
    void collect(Clock::time_point now = Clock::now(), bool execution_activated = false);
};
} // namespace iv
