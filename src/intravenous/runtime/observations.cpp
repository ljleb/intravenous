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
constexpr auto collection_interval = std::chrono::milliseconds(50);
constexpr std::size_t maximum_scopes = 64;
constexpr std::size_t maximum_interests_per_scope = 64;

bool same_capture(ObservationInterest const& interest, ObservationRequirement const& requirement)
{
    return interest.source == requirement.source && interest.operation == requirement.operation;
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
        if (std::ranges::any_of(desired.probes, [&](auto const& probe) { return same_capture(interest, probe); })) continue;
        auto old = std::ranges::find_if(accepted_.probes, [&](auto const& probe) { return same_capture(interest, probe); });
        desired.probes.push_back({.id = old == accepted_.probes.end() ? next_id_++ : old->id,
            .source = interest.source, .operation = interest.operation});
    }
    std::ranges::sort(desired.probes, {}, &ObservationRequirement::id);
    if (desired.probes.size() > maximum_live_observation_probes)
        throw std::invalid_argument("combined live observation limit exceeded");
    if (desired.probes == accepted_.probes) {
        if (!compilation_failed_) { last_compilation_us_ = 0; return false; }
        desired.revision = accepted_.revision;
    }
    ObservationGraphRequest request{.requirements = desired};
    IV_INVOKE_LINKER_EVENT(iv_observations_project_graph_event, request);
    if (!request.handled) throw std::runtime_error("project graph observation service is unavailable");
    // A failed compilation leaves the previous executor plan valid. Desired
    // requirements are retained for the next structural rebuild or retry.
    accepted_ = std::move(desired);
    last_compilation_us_ = request.compilation_us;
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
            reconcile_locked();
        } catch (...) {
            if (accepted_.revision == previous_revision) scopes_ = std::move(before);
            throw;
        }
        request.result = std::make_shared<Json const>(Json{
            {"revision", std::to_string(accepted_.revision)}, {"leaseMs", 30000},
            {"compilationUs", last_compilation_us_}});
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
        next_collection_ = now + collection_interval;
        auto const expired = std::erase_if(scopes_, [&](auto const& scope) { return scope.expires <= now; });
        requirements_dirty_ |= expired != 0;
        if (requirements_dirty_) {
            requirements_dirty_ = false;
            try { if (reconcile_locked()) return; } catch (...) { return; }
        }
        ObservationCollectRequest collection;
        IV_INVOKE_LINKER_EVENT(iv_observations_graph_executor_event, collection);
        if (collection.frame && collection.frame->activation_id >= last_activation_) {
            last_activation_ = collection.frame->activation_id;
            latest_ = std::move(collection.frame);
        }
        if (!latest_ || latest_->requirements_revision != accepted_.revision) return;
        std::map<std::string, Json> batches;
        for (auto const& scope : scopes_) {
            Json values = Json::array();
            for (auto const& interest : scope.interests) {
                auto requirement = std::ranges::find_if(accepted_.probes, [&](auto const& probe) { return same_capture(interest, probe); });
                if (requirement == accepted_.probes.end()) continue;
                auto const begin = latest_->values.begin();
                auto const end = begin + static_cast<std::ptrdiff_t>(latest_->count);
                auto value = std::find_if(begin, end, [&](auto const& item) { return item.request_id == requirement->id; });
                if (value == end) continue;
                Json entry{{"id", interest.id}, {"available", value->status == ObservationValueStatus::available
                    && value->value.captured}};
                if (entry["available"].get<bool>()) entry["value"] = project_value(*value);
                values.push_back(std::move(entry));
            }
            auto& batch = batches[scope.client];
            if (batch.is_null()) batch = Json::array();
            batch.push_back({{"scopeId", scope.scope}, {"values", std::move(values)}});
        }
        std::vector<ObservationDelivery> deliveries;
        for (auto& [client, scopes] : batches) {
            auto const signature = std::to_string(latest_->activation_id) + scopes.dump();
            if (last_delivered_[client] == signature) continue;
            last_delivered_[client] = signature;
            deliveries.push_back({client,
            std::make_shared<Json const>(Json{
                {"activationId", std::to_string(latest_->activation_id)},
                {"generation", std::to_string(latest_->project_generation)},
                {"revision", std::to_string(latest_->requirements_revision)},
                {"index", std::to_string(latest_->sample_index.signed_value())},
                {"blockSize", latest_->block_size},
                {"captureNs", latest_->measure_capture ? Json(latest_->capture_ns) : Json(nullptr)},
                {"scopes", std::move(scopes)}})});
        }
        if (!deliveries.empty()) IV_INVOKE_LINKER_EVENT(iv_observations_socket_rpc_event,
            std::span<ObservationDelivery const>{deliveries});
    }
}
} // namespace iv
