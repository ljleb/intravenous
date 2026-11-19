#include <intravenous/runtime/observations.h>
#include <intravenous/runtime/project_connection_json.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <nlohmann/json.hpp>

namespace iv {
namespace {
using Json = nlohmann::ordered_json;
constexpr auto scope_lease = std::chrono::seconds(30);
constexpr auto collection_interval = std::chrono::microseconds(33333);
constexpr std::size_t maximum_scopes = 64;
constexpr std::size_t maximum_interests_per_scope = 64;

bool same_capture(ObservationInterest const& interest, ObservationRequirement const& requirement)
{
    return !interest.window && interest.source == requirement.source && interest.operation == requirement.operation;
}
Json project_value(RawObservationValue const& value)
{
    Json result = Json::object();
    if (!value.operation || !value.operation->project) return result;
    auto data = value.operation->project(value.value);
    for (auto const& field : value.operation->result_fields) {
        if (field.kind == ObservationNumberKind::unsigned64) {
            std::uint64_t number;
            std::memcpy(&number, data.bytes.data() + field.byte_offset, sizeof(number));
            result[field.name] = std::to_string(number);
        } else {
            double number;
            if (field.kind == ObservationNumberKind::float32) {
                float scalar;
                std::memcpy(&scalar, data.bytes.data() + field.byte_offset, sizeof(scalar));
                number = scalar;
            } else std::memcpy(&number, data.bytes.data() + field.byte_offset, sizeof(number));
            result[field.name] = std::isfinite(number) ? Json(number) : Json(nullptr);
        }
    }
    return result;
}
} // namespace

bool Observations::reconcile_locked()
{
    ObservationRequirements desired{.revision = accepted_.revision + 1};
    for (auto const& scope : scopes_) for (auto const& interest : scope.interests) {
        if (interest.window) continue;
        if (std::ranges::any_of(desired.probes, [&](auto const& probe) { return same_capture(interest, probe); })) continue;
        auto old = std::ranges::find_if(accepted_.probes, [&](auto const& probe) { return same_capture(interest, probe); });
        desired.probes.push_back({.id = old == accepted_.probes.end() ? next_id_++ : old->id,
            .source = interest.source, .operation = interest.operation});
    }
    std::ranges::sort(desired.probes, {}, &ObservationRequirement::id);
    if (desired.probes.size() > maximum_live_observation_probes)
        throw std::invalid_argument("combined live observation limit exceeded");
    if (desired.probes == accepted_.probes) {
        if (!compilation_failed_) { last_scheduling_us_ = 0; return false; }
        desired.revision = accepted_.revision;
    }
    ObservationGraphRequest request{.requirements = desired};
    IV_INVOKE_LINKER_EVENT(iv_observations_project_graph_event, request);
    if (!request.handled) throw std::runtime_error("project graph observation service is unavailable");
    // A failed compilation leaves the previous executor plan valid. Desired
    // requirements are retained for the next structural rebuild or retry.
    accepted_ = std::move(desired);
    last_scheduling_us_ = request.scheduling_us;
    compilation_failed_ = !request.error.empty();
    if (!request.error.empty()) throw std::runtime_error(request.error);
    return true;
}

void Observations::handle_protocol(ObservationProtocolRequest& request)
{
    request.handled = true;
    try {
        if (request.nodes.size() > maximum_interests_per_scope)
            throw std::invalid_argument("too many observation capability sources");
        std::scoped_lock lock(mutex_);
        if (request.kind == ObservationProtocolRequest::Kind::describe) {
            ObservationGraphRequest query{.describe_nodes = request.nodes};
            IV_INVOKE_LINKER_EVENT(iv_observations_project_graph_event, query);
            if (!query.handled || !query.error.empty()) throw std::runtime_error(
                query.error.empty() ? "observation capabilities unavailable" : query.error);
            request.result = std::make_shared<Json const>(Json{{"capabilities", *query.capabilities}});
            return;
        }
        if (request.scope_id.empty() || request.scope_id.size() > 128
            || request.client_id.empty() || request.interests.size() > maximum_interests_per_scope)
            throw std::invalid_argument("invalid observation scope or interest count");
        std::set<std::string> ids;
        for (auto const& interest : request.interests) {
            if (interest.id.empty() || interest.id.size() > 128 || !ids.insert(interest.id).second
                || interest.source.member.empty() || interest.source.member.size() > 256
                || interest.operation.empty() || interest.operation.size() > 128)
                throw std::invalid_argument("invalid or duplicate observation interest");
            if (interest.window && (interest.source.kind == ObservationSourceKind::state_field
                || !interest.window->length || interest.window->length > static_cast<std::uint64_t>(INT64_MAX)
                || !interest.window->points || interest.window->points > maximum_observation_window_points))
                throw std::invalid_argument("invalid observation window");
        }
        auto scope = std::ranges::find_if(scopes_, [&](auto const& current) {
            return current.client == request.client_id && current.scope == request.scope_id;
        });
        auto before = scopes_;
        auto const previous_revision = accepted_.revision;
        if (request.interests.empty()) {
            if (scope != scopes_.end()) scopes_.erase(scope);
        } else {
            if (scope == scopes_.end()) {
                if (scopes_.size() >= maximum_scopes) throw std::invalid_argument("observation scope limit exceeded");
                scopes_.push_back({.client = request.client_id, .scope = request.scope_id});
                scope = std::prev(scopes_.end());
            }
            scope->interests = request.interests;
            scope->expires = Clock::now() + scope_lease;
        }
        last_delivered_.erase(request.client_id);
        try {
            std::size_t window_count = 0;
            for (auto const& current : scopes_) for (auto const& interest : current.interests)
                window_count += interest.window.has_value();
            if (window_count > maximum_live_observation_probes)
                throw std::invalid_argument("combined observation window limit exceeded");
            reconcile_locked();
        } catch (...) {
            if (accepted_.revision == previous_revision) scopes_ = std::move(before);
            throw;
        }
        request.result = std::make_shared<Json const>(Json{
            {"revision", std::to_string(accepted_.revision)}, {"leaseMs", 30000},
            {"schedulingUs", last_scheduling_us_}});
    } catch (std::exception const& error) { request.error = error.what(); }
}

void Observations::handle_disconnect(SocketRpcClientDisconnectedRequest& request)
{
    std::scoped_lock lock(mutex_);
    auto const erased = std::erase_if(scopes_, [&](auto const& scope) { return scope.client == request.client_id; });
    last_delivered_.erase(request.client_id);
    // ProjectGraph also handles this disconnect's manual-control cleanup.
    // Compile released observation requirements on the next collection cause,
    // so this RPC cause cannot enter ProjectGraph twice.
    requirements_dirty_ |= erased != 0;
}

void Observations::collect(Clock::time_point now)
{
    IV_EVENT_SOURCE(Observations, "live observation collection") {
        std::scoped_lock lock(mutex_);
        if (now < next_collection_) return;
        // Keep the cadence independent of the runtime service's 20 ms poll.
        // A late collection skips missed deadlines instead of sending a burst.
        next_collection_ += collection_interval;
        if (next_collection_ <= now) next_collection_ = now + collection_interval;
        auto const expired = std::erase_if(scopes_, [&](auto const& scope) { return scope.expires <= now; });
        requirements_dirty_ |= expired != 0;
        if (requirements_dirty_) {
            requirements_dirty_ = false;
            try { if (reconcile_locked()) return; } catch (...) { return; }
        }
        auto same_window = [](ObservationInterest const& left, ObservationInterest const& right) {
            return left.source == right.source && left.operation == right.operation && left.window == right.window;
        };
        std::vector<WindowCapture> desired_windows;
        for (auto const& scope : scopes_) for (auto const& interest : scope.interests) {
            if (!interest.window || std::ranges::any_of(desired_windows,
                [&](auto const& capture) { return same_window(capture.interest, interest); })) continue;
            auto existing = std::ranges::find_if(windows_,
                [&](auto const& capture) { return same_window(capture.interest, interest); });
            if (existing == windows_.end()) desired_windows.push_back({.interest = interest});
            else desired_windows.push_back(std::move(*existing));
        }
        for (auto const& previous : windows_) {
            if (previous.query) previous.query->canceled.store(true, std::memory_order_release);
        }
        windows_ = std::move(desired_windows);
        ObservationGraphRequest target;
        for (auto& capture : windows_) {
            if (capture.query && capture.query->complete.load(std::memory_order_acquire)) {
                auto const& result = capture.query->result;
                bool const outdated = capture.query->configured_generation != configured_generation_.value_or(0)
                    || ((result.status == ObservationWindowResult::Status::ready || result.status == ObservationWindowResult::Status::failed)
                        && result.data_version < execution_data_version_
                        && now >= capture.retry_after)
                    || (result.status == ObservationWindowResult::Status::pending_execution && now >= capture.retry_after);
                if (outdated) {
                    if (capture.query->configured_generation != configured_generation_.value_or(0)) {
                        capture.payload.reset(); capture.payload_query.reset();
                    }
                    capture.query->canceled.store(true, std::memory_order_release); capture.query.reset();
                }
            }
            if (!capture.query) {
                capture.query = std::make_shared<ObservationWindowQuery>();
                capture.query->window = *capture.interest.window;
                capture.submitted = false;
                capture.retry_after = now + std::chrono::milliseconds(250);
                target.windows.push_back({capture.interest.source, capture.interest.operation, capture.query});
            }
        }
        IV_INVOKE_LINKER_EVENT(iv_observations_project_graph_event, target);
        configured_generation_ = target.configured_generation;
        ObservationCollectRequest collection;
        for (auto& capture : windows_) {
            if (capture.query->configured_generation != configured_generation_.value_or(0)) {
                capture.query->canceled.store(true, std::memory_order_release);
                capture.query.reset();
                capture.payload.reset();
                capture.payload_query.reset();
                continue;
            }
            if (!capture.submitted && !capture.query->complete.load(std::memory_order_acquire)) {
                collection.windows.push_back(capture.query);
                capture.submitted = true;
            }
        }
        IV_INVOKE_LINKER_EVENT(iv_observations_graph_executor_event, collection);
        execution_data_version_ = collection.execution_data_version;
        if (!collection.handled) for (auto const& query : collection.windows) {
            query->result.status = ObservationWindowResult::Status::failed;
            query->result.error = "background observation executor is unavailable";
            query->complete.store(true, std::memory_order_release);
        }
        if (collection.frame && collection.frame->activation_id >= last_activation_) {
            last_activation_ = collection.frame->activation_id;
            latest_ = std::move(collection.frame);
        }
        bool const live_current = latest_ && latest_->requirements_revision == accepted_.revision;
        if (!live_current && windows_.empty()) return;
        bool const current_generation = live_current && (!target.handled || (target.configured_generation
            && *target.configured_generation == latest_->configured_generation));
        auto window_payload = [](WindowCapture& capture) -> Json const& {
            if (capture.payload_query == capture.query && capture.payload) return *capture.payload;
            capture.payload_query = capture.query;
            auto const& query = *capture.query;
            auto const& result = query.result;
            Json regions = Json::array();
            for (auto const& region : result.regions) {
                Json points = Json::array();
                for (auto const& [index, value] : region.values) {
                    Json point{{"index", std::to_string(index.signed_value())}, {"available", value.captured}};
                    if (value.captured) point["value"] = project_value({.operation = query.operation, .value = value});
                    points.push_back(std::move(point));
                }
                regions.push_back({{"begin", std::to_string(region.range.begin.signed_value())},
                    {"length", std::to_string(region.range.extent)}, {"points", std::move(points)}});
            }
            auto const status = result.status == ObservationWindowResult::Status::ready ? "ready"
                : result.status == ObservationWindowResult::Status::pending_execution ? "pending"
                : result.status == ObservationWindowResult::Status::unavailable ? "unavailable" : "failed";
            capture.payload = std::make_shared<Json const>(Json{
                {"window", {{"begin", std::to_string(query.window.begin.signed_value())},
                    {"length", std::to_string(query.window.length)}, {"points", query.window.points}}},
                {"status", status}, {"approximate", true}, {"truncated", result.truncated},
                {"semanticVersion", std::to_string(result.semantic_version)}, {"dataVersion", std::to_string(result.data_version)},
                {"sampleRate", result.sample_rate}, {"paceExponent", result.pace_exponent},
                {"executionGeneration", std::to_string(result.execution_generation)},
                {"regions", std::move(regions)}, {"error", result.error}});
            return *capture.payload;
        };
        std::map<std::string, Json> batches;
        for (auto const& scope : scopes_) {
            Json values = Json::array(), windows = Json::array();
            for (auto const& interest : scope.interests) {
                if (interest.window) {
                    auto capture = std::ranges::find_if(windows_,
                        [&](auto const& item) { return same_window(item.interest, interest); });
                    if (capture != windows_.end() && capture->query
                        && capture->query->complete.load(std::memory_order_acquire)) {
                        auto payload = window_payload(*capture);
                        payload["id"] = interest.id;
                        payload["outdated"] = capture->query->result.status == ObservationWindowResult::Status::ready
                            && capture->query->result.data_version < execution_data_version_;
                        windows.push_back(std::move(payload));
                    } else if (capture != windows_.end() && capture->payload
                        && (*capture->payload)["status"] == "ready") {
                        auto payload = *capture->payload;
                        payload["id"] = interest.id;
                        payload["outdated"] = true;
                        windows.push_back(std::move(payload));
                    } else {
                        windows.push_back({{"id", interest.id}, {"status", "pending"}, {"approximate", true},
                            {"window", {{"begin", std::to_string(interest.window->begin.signed_value())},
                                {"length", std::to_string(interest.window->length)}, {"points", interest.window->points}}},
                            {"regions", Json::array()}});
                    }
                    continue;
                }
                if (!live_current) continue;
                auto requirement = std::ranges::find_if(accepted_.probes, [&](auto const& probe) { return same_capture(interest, probe); });
                if (requirement == accepted_.probes.end()) continue;
                auto const begin = latest_->values.begin();
                auto const end = begin + static_cast<std::ptrdiff_t>(latest_->count);
                auto value = std::find_if(begin, end, [&](auto const& item) { return item.request_id == requirement->id; });
                if (value == end) continue;
                Json entry{{"id", interest.id}, {"available", current_generation && value->status == ObservationValueStatus::available
                    && value->value.captured}};
                // Configuration can be newer than the active executable. That
                // is pending work, not evidence that an observed source was
                // removed or became unavailable at an audio cutover.
                if (!current_generation && target.configured_generation) entry["pending"] = true;
                if (entry["available"].get<bool>()) entry["value"] = project_value(*value);
                values.push_back(std::move(entry));
            }
            auto& batch = batches[scope.client];
            if (batch.is_null()) batch = Json::array();
            batch.push_back({{"scopeId", scope.scope}, {"values", std::move(values)}, {"windows", std::move(windows)}});
        }
        std::vector<ObservationDelivery> deliveries;
        for (auto& [client, scopes] : batches) {
            auto const signature = std::to_string(last_activation_) + ":"
                + std::to_string(target.configured_generation.value_or(0)) + scopes.dump();
            if (last_delivered_[client] == signature) continue;
            last_delivered_[client] = signature;
            deliveries.push_back({client,
            std::make_shared<Json const>(Json{
                {"activationId", std::to_string(last_activation_)},
                {"generation", std::to_string(target.configured_generation.value_or(latest_ ? latest_->configured_generation : 0))},
                {"executionGeneration", std::to_string(latest_ ? latest_->project_generation : 0)},
                {"revision", std::to_string(accepted_.revision)},
                {"index", std::to_string(latest_ ? latest_->sample_index.signed_value() : 0)},
                {"blockSize", latest_ ? latest_->block_size : 0},
                {"captureNs", latest_ && latest_->measure_capture ? Json(latest_->capture_ns) : Json(nullptr)},
                {"scopes", std::move(scopes)}})});
        }
        if (!deliveries.empty()) IV_INVOKE_LINKER_EVENT(iv_observations_socket_rpc_event,
            std::span<ObservationDelivery const>{deliveries});
    }
}
} // namespace iv
