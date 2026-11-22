#include <intravenous/observations/observations.h>
#include <intravenous/observations/observation_delivery.h>
#include <intravenous/project/persistence/project_connection_json.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <ranges>
#include <set>
#include <utility>
#include <nlohmann/json.hpp>

namespace iv {
namespace {
using Json = nlohmann::ordered_json;
constexpr auto scope_lease = std::chrono::seconds(30);
constexpr auto collection_interval = std::chrono::microseconds(33333);
constexpr auto visible_window_retry = std::chrono::milliseconds(250);
constexpr auto prefetched_window_retry = std::chrono::seconds(2);
constexpr std::size_t maximum_scopes = 64;

Json observation_window_json(ObservationWindow const& window)
{
    Json result{{"begin", std::to_string(window.begin.signed_value())},
        {"length", std::to_string(window.length)}, {"points", window.points}};
    if (window.max_depth) result["maxDepth"] = *window.max_depth;
    if (window.include_indexed_runs) result["includeIndexedRuns"] = true;
    return result;
}

Json beat_run_json(ObservationBeatRun const& run)
{
    Json samples = Json::array();
    for (auto const sample : run.samples) samples.push_back(std::to_string(sample.signed_value()));
    return {{"source", run.source}, {"regionIndex", run.region_index},
        {"regionBegin", std::to_string(run.region.begin.signed_value())},
        {"regionLength", std::to_string(run.region.extent)},
        {"eventCount", std::to_string(run.event_count)}, {"depth", run.depth},
        {"firstEventIndex", std::to_string(run.indices.first)}, {"step", std::to_string(run.indices.step)},
        {"samples", std::move(samples)}};
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


Observations::Reconciliation Observations::reconcile_locked(
    std::size_t replaced_scope, std::optional<ScopeInterests> replacement)
{
    std::array<ScopeInterests, maximum_scopes> prospective;
    std::size_t count = 0;
    for (std::size_t index = 0; index < scopes_.size(); ++index) {
        if (index == replaced_scope) {
            if (replacement) prospective[count++] = *replacement;
        } else {
            auto const& scope = scopes_[index];
            prospective[count++] = {scope.interests, scope.predicted_interests, scope.prediction_sequence};
        }
    }
    if (replacement && replaced_scope == scopes_.size()) prospective[count++] = *replacement;
    auto const scopes = std::span(prospective).first(count);

    auto ids = active_probe_ids_;
    if (predicted_) for (auto const& probe : predicted_->probes) ids.emplace(ProbeKey{probe.source, probe.operation}, probe.id);
    auto next_id = next_id_;
    auto make_requirements = [&](ScopeInterests const* predicted_scope, std::uint64_t revision) {
        ObservationRequirements plan{.revision = revision};
        std::set<std::uint64_t> used;
        for (auto const& scope : scopes) {
            auto const& interests = &scope == predicted_scope ? scope.predicted_interests : scope.interests;
            for (auto const& interest : interests) {
                if (interest.window) continue;
                auto [entry, inserted] = ids.try_emplace({interest.source, interest.operation}, next_id);
                if (inserted) ++next_id;
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
    auto const owner = std::ranges::max_element(scopes, {}, [](ScopeInterests const& scope) {
        return scope.predicted_interests.empty() ? std::uint64_t{0} : scope.prediction_sequence;
    });
    if (owner != scopes.end() && !owner->predicted_interests.empty()) {
        auto const first_prediction_id = next_id;
        auto candidate = make_requirements(&*owner, desired.revision + 1);
        if (candidate.probes != desired.probes && candidate.probes.size() <= maximum_live_observation_probes)
            prediction = std::move(candidate);
        else next_id = first_prediction_id;
    }

    bool const required = changed || compilation_failed_;
    bool const prediction_changed = prediction != predicted_;
    if (!required && !prediction_changed) { last_scheduling_us_ = 0; return {}; }
    std::map<ProbeKey, std::uint64_t> accepted_ids;
    if (required) for (auto const& probe : desired.probes)
        accepted_ids.emplace(ProbeKey{probe.source, probe.operation}, probe.id);
    ObservationGraphRequest request{.requirements = required ? std::optional(desired) : std::nullopt,
        .prediction_changed = prediction_changed, .prediction = prediction};
    IV_INVOKE_LINKER_EVENT(iv_observations_project_graph_event, request);
    if (!request.handled) throw std::runtime_error("project graph observation service is unavailable");
    // A failed compilation leaves the previous executor plan valid. Desired
    // requirements are retained for the next structural rebuild or retry.
    if (required) {
        accepted_ = std::move(desired);
        active_probe_ids_ = std::move(accepted_ids);
    }
    bool const accept_scope = request.error.empty() || changed;
    if (accept_scope) {
        predicted_ = std::move(prediction);
        next_id_ = next_id;
    }
    last_scheduling_us_ = request.scheduling_us;
    compilation_failed_ = required && !request.error.empty();
    return {.dispatched = true, .accept_scope = accept_scope, .error = std::move(request.error)};
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
            auto const exact_events = interest.operation == recorded_event_timestamps_operation;
            auto const trigger_bins = interest.operation == recorded_trigger_bins_operation;
            if ((exact_events || trigger_bins) && (!interest.window || interest.source.kind != ObservationSourceKind::output
                || interest.source.port_kind != PortKind::event || interest.source.channel
                || (exact_events && interest.window->points) || interest.window->max_depth || interest.prefetch))
                throw std::invalid_argument("invalid recorded event observation");
            if (interest.window && (interest.source.kind == ObservationSourceKind::state_field
                || !interest.window->length || interest.window->length > static_cast<std::uint64_t>(INT64_MAX)
                || (!exact_events && (!interest.window->points || interest.window->points > maximum_observation_window_points))))
                throw std::invalid_argument("invalid observation window");
            if (interest.window && interest.window->include_indexed_runs
                && (interest.operation != "beat-grid" || !interest.window->max_depth))
                throw std::invalid_argument("indexed runs require a beat-grid observation");
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
        auto const* before = scope == scopes_.end() ? nullptr : std::addressof(*scope);
        bool const remove = request.interests.empty();
        if (!remove && !before && scopes_.size() >= maximum_scopes)
            throw std::invalid_argument("observation scope limit exceeded");
        auto const interests = std::span<ObservationInterest const>{request.interests};
        auto const previous_interests = before ? std::span<ObservationInterest const>{before->interests}
                                               : std::span<ObservationInterest const>{};
        auto const prediction = remove ? std::span<ObservationInterest const>{}
                                       : std::span<ObservationInterest const>{request.predicted_interests};
        auto const previous_prediction = before ? std::span<ObservationInterest const>{before->predicted_interests}
                                                : std::span<ObservationInterest const>{};
        bool const subscriptions_changed = !std::ranges::equal(previous_interests, interests);
        bool const prediction_changed = !std::ranges::equal(previous_prediction, prediction);
        auto live_only = std::views::filter([](ObservationInterest const& interest) { return !interest.window; });
        bool const plan_changed = prediction_changed || (subscriptions_changed
            && !std::ranges::equal(previous_interests | live_only, interests | live_only));
        if (subscriptions_changed) {
            auto const count_windows = [](std::span<ObservationInterest const> values) {
                return std::ranges::count_if(values, [](auto const& interest) { return interest.window.has_value(); });
            };
            std::size_t window_count = count_windows(interests);
            for (auto const& current : scopes_) {
                if (&current != before) window_count += count_windows(current.interests);
            }
            if (window_count > maximum_observation_windows)
                throw std::invalid_argument("combined observation window limit exceeded");
        }

        auto const expires = Clock::now() + scope_lease;
        auto const prediction_sequence = prediction_changed ? next_prediction_sequence_ + 1
            : (before ? before->prediction_sequence : 0);
        std::optional<Scope> new_scope;
        if (!remove && !before) {
            new_scope.emplace(Scope{.client = request.client_id, .scope = request.scope_id});
            if (scopes_.size() == scopes_.capacity())
                scopes_.reserve(std::min(maximum_scopes, std::max(std::size_t{1}, scopes_.capacity() * 2)));
        }
        Reconciliation reconciliation;
        if (plan_changed || requirements_dirty_ || compilation_failed_) {
            reconciliation = reconcile_locked(position, remove ? std::nullopt
                : std::optional(ScopeInterests{interests, prediction, prediction_sequence}));
        } else last_scheduling_us_ = 0;
        if (!reconciliation.accept_scope) throw std::runtime_error(reconciliation.error);

        if (remove) {
            if (position < scopes_.size()) scopes_.erase(scopes_.begin() + position);
        } else {
            if (new_scope) scopes_.push_back(std::move(*new_scope));
            auto& committed = scopes_[position];
            if (subscriptions_changed) committed.interests = std::move(request.interests);
            if (prediction_changed) {
                committed.predicted_interests = std::move(request.predicted_interests);
                committed.prediction_sequence = ++next_prediction_sequence_;
            }
            committed.expires = expires;
        }
        subscriptions_changed_ |= subscriptions_changed;
        if (subscriptions_changed) {
            if (auto delivered = last_delivered_.find(request.client_id); delivered != last_delivered_.end()) delivered->second.clear();
        }
        if (!reconciliation.error.empty()) throw std::runtime_error(reconciliation.error);
        request.result = std::make_shared<Json const>(Json{
            {"revision", std::to_string(accepted_.revision)},
            {"leaseMs", std::chrono::duration_cast<std::chrono::milliseconds>(scope_lease).count()},
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
                WindowKey{interest.source, interest.operation, window.begin.bits(), window.length,
                    window.points, window.max_depth, window.include_indexed_runs});
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

void Observations::run(std::stop_token stop)
{
    auto next = Clock::now();
    while (!stop.stop_requested()) {
        bool activated;
        {
            std::unique_lock lock(wake_mutex_);
            wake_.wait_until(lock, stop, next, [&] { return execution_activation_pending_; });
            if (stop.stop_requested()) return;
            activated = std::exchange(execution_activation_pending_, false);
        }
        next = collect(std::nullopt, activated);
    }
}

void Observations::notify_execution_activation()
{
    {
        std::scoped_lock lock(wake_mutex_);
        execution_activation_pending_ = true;
    }
    wake_.notify_one();
}

Observations::Clock::time_point Observations::collect(
    std::optional<Clock::time_point> time, bool execution_activated)
{
    std::scoped_lock lock(mutex_);
    auto const now = time ? *time : Clock::now();
    auto const delivery_due = now >= next_collection_;
    if (!delivery_due && !execution_activated) return next_collection_;
    // Skip missed deliveries. Activation can submit windows early and leaves
    // the delivery deadline in place.
    if (delivery_due) next_collection_ = now + collection_interval;
    if (scopes_.empty() && last_delivered_.empty() && !requirements_dirty_ && !subscriptions_changed_)
        return next_collection_;
    IV_EVENT_SOURCE(Observations, "live observation collection") {
        if (refresh_subscriptions_locked(now)) {
            auto const execution = collect_execution_locked(now);
            if (delivery_due) publish_deliveries_locked(execution);
        }
    }
    return next_collection_;
}

bool Observations::refresh_subscriptions_locked(Clock::time_point now)
{
    auto const expired = std::erase_if(scopes_, [&](auto const& scope) { return scope.expires <= now; });
    requirements_dirty_ |= expired != 0;
    subscriptions_changed_ |= expired != 0;
    if (requirements_dirty_) {
        requirements_dirty_ = false;
        // Reconciliation and window resolution both visit ProjectGraph.
        // A change to the prediction alone also uses this cause's visit.
        try { if (reconcile_locked(scopes_.size(), std::nullopt).dispatched) return false; } catch (...) { return false; }
    }
    if (subscriptions_changed_) update_subscriptions_locked();
    return true;
}

Observations::CollectedExecution Observations::collect_execution_locked(Clock::time_point now)
{
    ObservationGraphRequest target;
    target.windows.reserve(window_order_.size());
    for (auto* pointer : window_order_) {
        auto& capture = *pointer;
        if (capture.query && capture.query->complete.load(std::memory_order_acquire)) {
            auto const& result = capture.query->result;
            auto const retry_interval = capture.interest.prefetch ? prefetched_window_retry : visible_window_retry;
            bool const retry_due = now >= capture.requested_at + retry_interval;
            bool const outdated = ((result.status == ObservationWindowResult::Status::ready || result.status == ObservationWindowResult::Status::failed)
                    && result.data_version < execution_data_version_ && retry_due)
                || (result.status == ObservationWindowResult::Status::pending_execution && retry_due);
            if (outdated) {
                capture.query->canceled.store(true, std::memory_order_release); capture.query.reset();
            }
        }
        if (capture.query) capture.query->prefetch.store(capture.interest.prefetch, std::memory_order_relaxed);
        target.windows.push_back({capture.interest.source, capture.interest.operation, *capture.interest.window,
            capture.interest.prefetch, capture.query});
    }
    IV_INVOKE_LINKER_EVENT(iv_observations_project_graph_event, target);
    bool const checkpoint_changed = window_configured_generation_ != target.window_configured_generation
        || window_execution_generation_ != target.window_execution_generation;
    window_configured_generation_ = target.window_configured_generation;
    window_execution_generation_ = target.window_execution_generation;
    ObservationCollectRequest collection;
    for (std::size_t i = 0; i < window_order_.size(); ++i) {
        auto& capture = *window_order_[i];
        auto& resolved = target.windows[i];
        if (checkpoint_changed) {
            capture.payload.reset(); capture.body.reset(); capture.payload_query.reset();
        }
        if (capture.query != resolved.query) {
            if (capture.query) capture.query->canceled.store(true, std::memory_order_release);
            capture.query = std::move(resolved.query);
            capture.submitted = false;
            capture.requested_at = now;
        }
        if (capture.query && !capture.submitted && !capture.query->complete.load(std::memory_order_acquire)) {
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
    bool const current_generation = live_current && (!target.handled || (window_configured_generation_
        && *window_configured_generation_ == latest_->configured_generation
        && (!window_execution_generation_ || *window_execution_generation_ == latest_->project_generation)));
    return {.sample_rate = target.window_sample_rate, .live_available = current_generation};
}

Json const& Observations::window_payload_locked(WindowCapture& capture)
{
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
        {"window", observation_window_json(query.window)},
        {"status", status}, {"approximate", true}, {"truncated", result.truncated},
        {"semanticVersion", std::to_string(result.semantic_version)}, {"dataVersion", std::to_string(result.data_version)},
        {"sampleRate", result.sample_rate}, {"paceExponent", result.pace_exponent},
        {"executionGeneration", std::to_string(result.execution_generation)}, {"error", result.error}};
    Json body{{"regions", std::move(regions)}};
    if (capture.interest.operation == recorded_event_timestamps_operation) {
        Json indices = Json::array();
        for (auto const index : result.event_indices)
            indices.push_back(std::to_string(index.signed_value()));
        payload["approximate"] = false;
        body["eventIndices"] = std::move(indices);
    } else if (capture.interest.operation == recorded_trigger_bins_operation) {
        Json bins = Json::array();
        for (auto const& bin : result.trigger_bins) {
            bins.push_back({{"begin", std::to_string(bin.range.begin.signed_value())},
                {"length", std::to_string(bin.range.extent)},
                {"first", std::to_string(bin.first.signed_value())}, {"count", std::to_string(bin.count)}});
        }
        body["triggerBins"] = std::move(bins);
    } else if (query.window.max_depth) {
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
        body["marks"] = std::move(marks);
        if (query.window.include_indexed_runs) {
            Json runs = Json::array();
            for (auto const& run : result.beat_runs) runs.push_back(beat_run_json(run));
            body["beatRuns"] = std::move(runs);
        }
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
}

void Observations::publish_deliveries_locked(CollectedExecution const& execution)
{
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
                    auto payload = window_payload_locked(*capture);
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
                    windows.push_back({{"id", interest.id}, {"status", "pending"},
                        {"approximate", interest.source.port_kind != PortKind::event},
                        {"window", observation_window_json(*interest.window)}});
                }
                continue;
            }
            if (!execution.live_available) {
                values.push_back({{"id", interest.id}, {"available", false}, {"pending", true}});
                continue;
            }
            auto const found = live_capture_indices_.find(scope.captures[i].probe_id);
            if (found == live_capture_indices_.end()) continue;
            auto const& value = latest_->values[found->second];
            Json entry{{"id", interest.id}, {"available", value.status == ObservationValueStatus::available && value.value.captured}};
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
            + std::to_string(window_configured_generation_.value_or(0)) + ":"
            + std::to_string(window_execution_generation_.value_or(0)) + ":"
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
            {"generation", std::to_string(window_configured_generation_.value_or(latest_ ? latest_->configured_generation : 0))},
            {"executionGeneration", std::to_string(window_execution_generation_.value_or(latest_ ? latest_->project_generation : 0))},
            {"sampleRate", execution.sample_rate ? Json(*execution.sample_rate) : Json(nullptr)},
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
} // namespace iv
