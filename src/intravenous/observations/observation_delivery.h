#pragma once

#include <intravenous/observations/observation_events.h>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace iv {
struct ObservationWindowBody {
    std::string id;
    nlohmann::ordered_json data;
};

class ObservationDeliveryState {
    using Json = nlohmann::ordered_json;
    using Index = std::unordered_map<std::string_view, Json const*>;
    std::shared_ptr<Json const> previous_;

    static Index index(Json const& entries, char const* key)
    {
        Index indexed;
        for (auto const& entry : entries) indexed.emplace(entry.at(key).get_ref<std::string const&>(), &entry);
        return indexed;
    }

    static Json fields(Json const& current, Json const& previous)
    {
        auto patch = Json::object();
        for (auto const& [key, value] : current.items()) {
            if (key == "id" || key == "bodyId") continue;
            auto const old = previous.find(key);
            if (old == previous.end() || *old != value) patch[key] = value;
        }
        for (auto const& [key, value] : previous.items()) {
            if (key != "id" && key != "bodyId" && !current.contains(key)) patch[key] = nullptr;
        }
        return patch;
    }

    template<class Patch>
    static Json entries(Json const& current, Json const& previous, char const* key, Patch&& difference)
    {
        auto old = index(previous, key);
        auto patch = Json::object();
        for (auto const& entry : current) {
            auto const& id = entry.at(key).get_ref<std::string const&>();
            auto const found = old.find(id);
            auto change = difference(entry, found != old.end() ? found->second : nullptr);
            if (!change.is_null()) patch[id] = std::move(change);
            if (found != old.end()) old.erase(found);
        }
        for (auto [id, entry] : old) patch[std::string(id)] = nullptr;
        return patch;
    }
public:
    // Call only for a frame selected by the socket writer. A failed write
    // closes the connection, so no later frame can depend on that write.
    nlohmann::ordered_json payload_for_write(ObservationDelivery const& delivery)
    {
        auto const& current = *delivery.payload;
        bool const snapshot = !previous_ || current.at("generation") != previous_->at("generation");
        auto const empty = Json::array();
        auto const no_fields = Json::object();
        std::unordered_map<std::string_view, Json const*> bodies;
        for (auto const& body : delivery.window_bodies) bodies.emplace(body->id, &body->data);
        auto payload = Json::object();
        for (auto const& [key, value] : current.items()) if (key != "scopes") payload[key] = value;
        payload["snapshot"] = snapshot;
        payload["scopes"] = entries(current.at("scopes"), snapshot ? empty : previous_->at("scopes"), "scopeId",
            [&](Json const& scope, Json const* old) -> Json {
                auto values = entries(scope.at("values"), old ? old->at("values") : empty, "id",
                    [&](Json const& value, Json const* before) -> Json {
                        return before && value == *before ? Json(nullptr) : fields(value, no_fields);
                    });
                auto windows = entries(scope.at("windows"), old ? old->at("windows") : empty, "id",
                    [&](Json const& window, Json const* before) -> Json {
                        auto patch = fields(window, before ? *before : no_fields);
                        auto const id = window.find("bodyId");
                        auto const old_id = before ? before->find("bodyId") : no_fields.end();
                        bool const had_body = before && old_id != before->end();
                        if (id != window.end()) {
                            if (!had_body || *id != *old_id) patch["body"] = *bodies.at(id->get_ref<std::string const&>());
                        } else if (had_body) patch["body"] = nullptr;
                        return patch.empty() ? Json(nullptr) : patch;
                    });
                if (old && values.empty() && windows.empty()) return nullptr;
                auto patch = Json::object();
                if (!values.empty()) patch["values"] = std::move(values);
                if (!windows.empty()) patch["windows"] = std::move(windows);
                return patch;
            });
        previous_ = delivery.payload;
        return payload;
    }
};
} // namespace iv
