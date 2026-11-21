#include <intravenous/runtime/observations.h>
#include <intravenous/runtime/observation_delivery.h>
#include <intravenous/runtime/project_connection_json.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <ranges>
#include <set>
#include <nlohmann/json.hpp>

namespace iv {
namespace {
using Json = nlohmann::ordered_json;
constexpr auto scope_lease = std::chrono::seconds(30);
constexpr auto collection_interval = std::chrono::microseconds(33333);
constexpr std::size_t maximum_scopes = 64;

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
    auto ids = active_probe_ids_;
    if (predicted_) for (auto const& probe : predicted_->probes) ids.emplace(ProbeKey{probe.source, probe.operation}, probe.id);
    auto make_requirements = [&](Scope const* predicted_scope, std::uint64_t revision) {
        ObservationRequirements plan{.revision = revision};
        std::set<std::uint64_t> used;
        for (auto const& scope : scopes_) {
            auto const& interests = &scope == predicted_scope ? scope.predicted_interests : scope.interests;
            for (auto const& interest : interests) {
                if (interest.window) continue;
                auto [entry, inserted] = ids.try_emplace({interest.source, interest.operation}, next_id_);
                if (inserted) ++next_id_;
                if (used.insert(entry->second).second)
                    plan.probes.push_back({.id = entry->second, .source = interest.source, .operation = interest.operation});
            }
        }
        std::ranges::sort(plan.probes, {}, &ObservationRequirement::id);
        return plan;
    };

    auto desired = make_requirements(nullptr, accepted_.revision + 1);
    if (desired.probes.size() > maximum_live_observation_probes)
        throw std::invalid_argument("combined live observation limit exceeded");
    bool const changed = desired.probes != accepted_.probes;
    if (!changed) desired.revision = accepted_.revision;
    std::optional<ObservationRequirements> prediction;
    auto const owner = std::ranges::max_element(scopes_, {}, [](Scope const& scope) {
        return scope.predicted_interests.empty() ? std::uint64_t{0} : scope.prediction_sequence;
    });
    if (owner != scopes_.end() && !owner->predicted_interests.empty()) {
        auto candidate = make_requirements(&*owner, desired.revision + 1);
        if (candidate.probes != desired.probes && candidate.probes.size() <= maximum_live_observation_probes)
            prediction = std::move(candidate);
    }

    bool const required = changed || compilation_failed_;
    bool const prediction_changed = prediction != predicted_;
    if (!required && !prediction_changed) { last_scheduling_us_ = 0; return false; }
    ObservationGraphRequest request{.requirements = required ? std::optional(desired) : std::nullopt,
        .prediction_changed = prediction_changed, .prediction = prediction};
    IV_INVOKE_LINKER_EVENT(iv_observations_project_graph_event, request);
    if (!request.handled) throw std::runtime_error("project graph observation service is unavailable");
    // A failed compilation leaves the previous executor plan valid. Desired
    // requirements are retained for the next structural rebuild or retry.
    if (required) {
        accepted_ = std::move(desired);
        active_probe_ids_.clear();
        for (auto const& probe : accepted_.probes) active_probe_ids_.emplace(ProbeKey{probe.source, probe.operation}, probe.id);
    }
    predicted_ = std::move(prediction);
    last_scheduling_us_ = request.scheduling_us;
    compilation_failed_ = required && !request.error.empty();
    if (!request.error.empty()) throw std::runtime_error(request.error);
    return required;
}

void Observations::handle_protocol(ObservationProtocolRequest& request)
{
    request.handled = true;
    try {
        if (request.nodes.size() > maximum_observation_interests)
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
            || request.client_id.empty() || request.interests.size() > maximum_observation_interests
            || request.predicted_interests.size() > maximum_live_observation_probes)
            throw std::invalid_argument("invalid observation scope or interest count");
        std::set<std::string> ids;
        auto validate = [&](ObservationInterest const& interest) {
            if (interest.id.empty() || interest.id.size() > 128 || !ids.insert(interest.id).second
                || (interest.source.member.empty() && interest.source.kind == ObservationSourceKind::state_field)
                || interest.source.member.size() > 256
                || interest.operation.empty() || interest.operation.size() > 128)
                throw std::invalid_argument("invalid or duplicate observation interest");
            if (interest.prefetch && !interest.window) throw std::invalid_argument("observation prefetch requires a window");
            if (interest.window && (interest.source.kind == ObservationSourceKind::state_field
                || !interest.window->length || interest.window->length > static_cast<std::uint64_t>(INT64_MAX)
                || !interest.window->points || interest.window->points > maximum_observation_window_points))
                throw std::invalid_argument("invalid observation window");
        };
        for (auto const& interest : request.interests) validate(interest);
        ids.clear();
        for (auto const& interest : request.predicted_interests) {
            validate(interest);
            if (interest.window || interest.source.port_kind == PortKind::event)
                throw std::invalid_argument("observation predictions require live probes");
        }
        auto scope = std::ranges::find_if(scopes_, [&](auto const& current) {
            return current.client == request.client_id && current.scope == request.scope_id;
        });
        auto const position = static_cast<std::size_t>(scope - scopes_.begin());
        auto before = scope == scopes_.end() ? std::nullopt : std::optional<Scope>(*scope);
        auto const previous_revision = accepted_.revision;
        bool subscriptions_changed = false;
        bool plan_changed = false;
        auto live_only = std::views::filter([](ObservationInterest const& interest) { return !interest.window; });
        if (request.interests.empty()) {
            if (scope != scopes_.end()) {
                plan_changed = !std::ranges::empty(scope->interests | live_only) || !scope->predicted_interests.empty();
                scopes_.erase(scope);
                subscriptions_changed = true;
            }
        } else {
            if (scope == scopes_.end()) {
                if (scopes_.size() >= maximum_scopes) throw std::invalid_argument("observation scope limit exceeded");
                scopes_.push_back({.client = request.client_id, .scope = request.scope_id});
                scope = std::prev(scopes_.end());
            }
            subscriptions_changed = scope->interests != request.interests;
            if (subscriptions_changed) {
                plan_changed = !std::ranges::equal(scope->interests | live_only, request.interests | live_only);
                scope->interests = request.interests;
            }
            if (scope->predicted_interests != request.predicted_interests) {
                scope->predicted_interests = request.predicted_interests;
                scope->prediction_sequence = ++next_prediction_sequence_;
                plan_changed = true;
            }
            scope->expires = Clock::now() + scope_lease;
        }
        try {
            std::size_t window_count = 0;
            for (auto const& current : scopes_) for (auto const& interest : current.interests)
                window_count += interest.window.has_value();
            if (window_count > maximum_observation_windows)
                throw std::invalid_argument("combined observation window limit exceeded");
            if (plan_changed || requirements_dirty_ || compilation_failed_) reconcile_locked();
            else last_scheduling_us_ = 0;
        } catch (...) {
            if (accepted_.revision == previous_revision) {
                if (before) {
                    if (request.interests.empty()) scopes_.insert(scopes_.begin() + position, std::move(*before));
                    else scopes_[position] = std::move(*before);
                } else if (!request.interests.empty()) scopes_.pop_back();
            } else subscriptions_changed_ |= subscriptions_changed;
            throw;
        }
        subscriptions_changed_ |= subscriptions_changed;
        if (subscriptions_changed) {
            if (auto delivered = last_delivered_.find(request.client_id); delivered != last_delivered_.end()) delivered->second.clear();
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
    subscriptions_changed_ |= erased != 0;
}

void Observations::update_subscriptions_locked()
{
    for (auto& [key, capture] : windows_) capture.subscribers = 0;
    window_order_.clear();
    for (auto& scope : scopes_) {
        scope.captures.resize(scope.interests.size());
        for (std::size_t i = 0; i < scope.interests.size(); ++i) {
            auto const& interest = scope.interests[i];
            if (!interest.window) {
                auto const probe = active_probe_ids_.find({interest.source, interest.operation});
                scope.captures[i] = {.probe_id = probe == active_probe_ids_.end() ? 0 : probe->second};
                continue;
            }
            auto const& window = *interest.window;
            auto [position, inserted] = windows_.try_emplace(
                WindowKey{interest.source, interest.operation, window.begin.bits(), window.length, window.points, window.max_depth});
            auto& capture = position->second;
            if (inserted) capture.interest = interest;
            if (capture.subscribers++ == 0) {
                window_order_.push_back(&capture);
                capture.interest.prefetch = interest.prefetch;
            } else capture.interest.prefetch &= interest.prefetch;
            scope.captures[i] = {.window = &capture};
        }
    }
    std::erase_if(windows_, [](auto const& entry) {
        auto const& capture = entry.second;
        if (capture.subscribers) return false;
        if (capture.query) capture.query->canceled.store(true, std::memory_order_release);
        return true;
    });
    subscriptions_changed_ = false;
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
        subscriptions_changed_ |= expired != 0;
        if (requirements_dirty_) {
            requirements_dirty_ = false;
            try { if (reconcile_locked()) return; } catch (...) { return; }
        }
        if (subscriptions_changed_) update_subscriptions_locked();
        ObservationGraphRequest target;
        for (auto* pointer : window_order_) {
            auto& capture = *pointer;
            if (capture.query && capture.query->complete.load(std::memory_order_acquire)) {
                auto const& result = capture.query->result;
                auto const retry_interval = capture.interest.prefetch ? std::chrono::milliseconds(2000) : std::chrono::milliseconds(250);
                bool const retry_due = now >= capture.requested_at + retry_interval;
                bool const outdated = capture.query->configured_generation != configured_generation_.value_or(0)
                    || ((result.status == ObservationWindowResult::Status::ready || result.status == ObservationWindowResult::Status::failed)
                        && result.data_version < execution_data_version_ && retry_due)
                    || (result.status == ObservationWindowResult::Status::pending_execution && retry_due);
                if (outdated) {
                    if (capture.query->configured_generation != configured_generation_.value_or(0)) {
                        capture.payload.reset(); capture.body.reset(); capture.payload_query.reset();
                    }
                    capture.query->canceled.store(true, std::memory_order_release); capture.query.reset();
                }
            }
            if (!capture.query) {
                capture.query = std::make_shared<ObservationWindowQuery>();
                capture.query->window = *capture.interest.window;
                capture.submitted = false;
                capture.requested_at = now;
                target.windows.push_back({capture.interest.source, capture.interest.operation, capture.query});
            }
            capture.query->prefetch.store(capture.interest.prefetch, std::memory_order_relaxed);
        }
        IV_INVOKE_LINKER_EVENT(iv_observations_project_graph_event, target);
        configured_generation_ = target.configured_generation;
        ObservationCollectRequest collection;
        for (auto* pointer : window_order_) {
            auto& capture = *pointer;
            if (capture.query->configured_generation != configured_generation_.value_or(0)) {
                capture.query->canceled.store(true, std::memory_order_release);
                capture.query.reset();
                capture.payload.reset();
                capture.body.reset();
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
            auto const& frame = *collection.frame;
            if (!latest_ || latest_->project_generation != frame.project_generation
                || latest_->requirements_revision != frame.requirements_revision || latest_->count != frame.count) {
                live_capture_indices_.clear();
                for (std::size_t i = 0; i < frame.count; ++i) live_capture_indices_.emplace(frame.values[i].request_id, i);
            }
            last_activation_ = frame.activation_id;
            latest_ = std::move(collection.frame);
        }
        bool const live_current = latest_ && latest_->requirements_revision == accepted_.revision;
        bool const current_generation = live_current && (!target.handled || (target.configured_generation
            && *target.configured_generation == latest_->configured_generation));
        auto window_payload = [&](WindowCapture& capture) -> Json const& {
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
            Json payload{
                {"window", {{"begin", std::to_string(query.window.begin.signed_value())},
                    {"length", std::to_string(query.window.length)}, {"points", query.window.points}}},
                {"status", status}, {"approximate", true}, {"truncated", result.truncated},
                {"semanticVersion", std::to_string(result.semantic_version)}, {"dataVersion", std::to_string(result.data_version)},
                {"sampleRate", result.sample_rate}, {"paceExponent", result.pace_exponent},
                {"executionGeneration", std::to_string(result.execution_generation)}, {"error", result.error}};
            Json body{{"regions", std::move(regions)}};
            if (query.port.port_kind == PortKind::event || query.window.max_depth) {
                Json marks = Json::array(), unavailable = Json::array();
                for (auto const& event : result.events) {
                    marks.push_back({{"index", std::to_string(event.time.signed_value())},
                        {"depth", std::get<BeatEvent>(event.value).depth}});
                }
                for (auto const& range : result.unavailable_regions) {
                    unavailable.push_back({{"begin", std::to_string(range.begin.signed_value())},
                        {"length", std::to_string(range.extent)}});
                }
                payload["approximate"] = false;
                payload["window"]["maxMarks"] = query.window.points;
                payload["window"]["maxDepth"] = query.window.max_depth.value_or(0);
                body["marks"] = std::move(marks);
                body["unavailableRegions"] = std::move(unavailable);
            }
            if (result.status == ObservationWindowResult::Status::ready) {
                if (!capture.body || capture.body->data != body)
                    capture.body = std::make_shared<ObservationWindowBody const>(ObservationWindowBody{
                        std::to_string(next_window_body_id_++), std::move(body)});
                payload["bodyId"] = capture.body->id;
            } else capture.body.reset();
            capture.payload = std::make_shared<Json const>(std::move(payload));
            return *capture.payload;
        };
        struct Batch {
            Json scopes = Json::array();
            std::vector<std::shared_ptr<ObservationWindowBody const>> bodies;
        };
        std::map<std::string, Batch> batches;
        for (auto const& [client, signature] : last_delivered_) batches.try_emplace(client);
        for (auto const& scope : scopes_) {
            auto& batch = batches[scope.client];
            Json values = Json::array(), windows = Json::array();
            for (std::size_t i = 0; i < scope.interests.size(); ++i) {
                auto const& interest = scope.interests[i];
                if (interest.window) {
                    auto* capture = scope.captures[i].window;
                    bool const has_ready_payload = capture->payload
                        && (*capture->payload)["status"] == "ready";
                    // A refresh can finish pending while playback prepares a seek.
                    // Keep the ready window until the retry produces a result.
                    if (capture->query
                        && capture->query->complete.load(std::memory_order_acquire)
                        && (!has_ready_payload
                            || capture->query->result.status != ObservationWindowResult::Status::pending_execution)) {
                        auto payload = window_payload(*capture);
                        payload["id"] = interest.id;
                        payload["outdated"] = capture->query->result.status == ObservationWindowResult::Status::ready
                            && capture->query->result.data_version < execution_data_version_;
                        windows.push_back(std::move(payload));
                        if (capture->body) batch.bodies.push_back(capture->body);
                    } else if (has_ready_payload) {
                        auto payload = *capture->payload;
                        payload["id"] = interest.id;
                        payload["outdated"] = true;
                        windows.push_back(std::move(payload));
                        batch.bodies.push_back(capture->body);
                    } else {
                        windows.push_back({{"id", interest.id}, {"status", "pending"}, {"approximate", true},
                            {"window", {{"begin", std::to_string(interest.window->begin.signed_value())},
                                {"length", std::to_string(interest.window->length)}, {"points", interest.window->points}}}});
                    }
                    continue;
                }
                if (!live_current) {
                    values.push_back({{"id", interest.id}, {"available", false}, {"pending", true}});
                    continue;
                }
                auto const found = live_capture_indices_.find(scope.captures[i].probe_id);
                if (found == live_capture_indices_.end()) continue;
                auto const& value = latest_->values[found->second];
                Json entry{{"id", interest.id}, {"available", current_generation && value.status == ObservationValueStatus::available
                    && value.value.captured}};
                // Configuration can be newer than the active executable. That
                // is pending work, not evidence that an observed source was
                // removed or became unavailable at an audio cutover.
                if (!current_generation && target.configured_generation) entry["pending"] = true;
                if (entry["available"].get<bool>()) entry["value"] = project_value(value);
                values.push_back(std::move(entry));
            }
            batch.scopes.push_back({{"scopeId", scope.scope}, {"values", std::move(values)}, {"windows", std::move(windows)}});
        }
        std::vector<ObservationDelivery> deliveries;
        for (auto& [client, batch] : batches) {
            auto& scopes = batch.scopes;
            bool const has_windows = std::ranges::any_of(scopes, [](auto const& scope) { return !scope["windows"].empty(); });
            auto const signature = std::to_string(last_activation_) + ":"
                + std::to_string(target.configured_generation.value_or(0)) + ":"
                + std::to_string(accepted_.revision) + ":"
                + std::to_string(has_windows ? execution_data_version_ : 0) + scopes.dump();
            if (scopes.empty()) last_delivered_.erase(client);
            else {
                if (last_delivered_[client] == signature) continue;
                last_delivered_[client] = signature;
            }
            deliveries.push_back({client,
            std::make_shared<Json const>(Json{
                {"activationId", std::to_string(last_activation_)},
                {"generation", std::to_string(target.configured_generation.value_or(latest_ ? latest_->configured_generation : 0))},
                {"executionGeneration", std::to_string(latest_ ? latest_->project_generation : 0)},
                {"dataVersion", std::to_string(execution_data_version_)},
                {"revision", std::to_string(accepted_.revision)},
                {"index", std::to_string(latest_ ? latest_->sample_index.signed_value() : 0)},
                {"blockSize", latest_ ? latest_->block_size : 0},
                {"captureNs", latest_ && latest_->measure_capture ? Json(latest_->capture_ns) : Json(nullptr)},
                {"scopes", std::move(scopes)}}), std::move(batch.bodies)});
        }
        if (!deliveries.empty()) IV_INVOKE_LINKER_EVENT(iv_observations_socket_rpc_event,
            std::span<ObservationDelivery const>{deliveries});
    }
}
} // namespace iv
