#pragma once

#include "sample.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
    class GraphBuilder;
    struct InputConfig;
    struct EventInputConfig;
    struct SamplePortRef;
    struct EventPortRef;
    class Timeline;

    class TimelineAugmentation {
        Timeline* _timeline;
        std::unordered_map<std::string, size_t> _control_node_indices;

    public:
        TimelineAugmentation() = default;
        explicit TimelineAugmentation(Timeline& timeline) :
            _timeline(&timeline)
        {}

        SamplePortRef resolve_sample_input(
            GraphBuilder& builder,
            std::string_view logical_node_id,
            std::optional<size_t> member_ordinal,
            size_t input_ordinal,
            InputConfig const& input_config
        );

        EventPortRef resolve_event_input(
            GraphBuilder& builder,
            std::string_view logical_node_id,
            size_t input_ordinal,
            EventInputConfig const& input_config
        );
    };

    class Timeline {
        mutable std::mutex _live_input_bindings_mutex;
        std::unordered_map<std::string, std::vector<std::vector<Sample*>>> _live_inputs;
        std::unordered_map<std::string, std::vector<Sample>> _live_input_values;
        std::unordered_set<std::string> _concrete_live_input_overrides;

        static std::string concrete_key(std::string_view logical_node_id, size_t member_ordinal)
        {
            return concrete_key_prefix(logical_node_id) + std::to_string(member_ordinal);
        }

        static std::string concrete_key_prefix(std::string_view logical_node_id)
        {
            return std::string(logical_node_id) + "\x1fmember:";
        }

        static std::string concrete_override_key(
            std::string_view logical_node_id,
            size_t member_ordinal,
            size_t input_ordinal
        )
        {
            return concrete_key(logical_node_id, member_ordinal) + "\x1finput:" + std::to_string(input_ordinal);
        }

        std::vector<Sample*>& ensure_live_input_slots_locked(
            std::string_view key,
            size_t input_ordinal
        )
        {
            auto& slots = _live_inputs[std::string(key)];
            if (slots.size() <= input_ordinal) {
                slots.resize(input_ordinal + 1);
            }
            return slots[input_ordinal];
        }

        Sample& ensure_live_input_value_locked(
            std::string_view logical_node_id,
            size_t input_ordinal
        )
        {
            auto& values = _live_input_values[std::string(logical_node_id)];
            if (values.size() <= input_ordinal) {
                values.resize(input_ordinal + 1);
            }
            return values[input_ordinal];
        }

        friend class TimelineAugmentation;

    public:
        TimelineAugmentation begin_augmentation()
        {
            return TimelineAugmentation(*this);
        }

        void register_live_input_value(
            std::string_view logical_node_id,
            std::optional<size_t> member_ordinal,
            size_t input_ordinal,
            Sample* value_location
        )
        {
            std::scoped_lock lock(_live_input_bindings_mutex);
            auto const key = member_ordinal.has_value()
                ? concrete_key(logical_node_id, *member_ordinal)
                : std::string(logical_node_id);
            auto& slots = ensure_live_input_slots_locked(key, input_ordinal);
            if (std::ranges::find(slots, value_location) == slots.end()) {
                slots.push_back(value_location);
            }

            Sample value = *value_location;
            if (member_ordinal.has_value()) {
                auto const override = concrete_override_key(logical_node_id, *member_ordinal, input_ordinal);
                if (_concrete_live_input_overrides.contains(override)) {
                    value = ensure_live_input_value_locked(key, input_ordinal);
                } else {
                    value = live_input_value_or_locked(logical_node_id, input_ordinal, *value_location);
                }
            } else {
                value = live_input_value_or_locked(logical_node_id, input_ordinal, *value_location);
                ensure_live_input_value_locked(logical_node_id, input_ordinal) = value;
            }
            *value_location = value;
        }

        void move_live_input_value(
            std::string_view logical_node_id,
            std::optional<size_t> member_ordinal,
            size_t input_ordinal,
            Sample* previous_location,
            Sample* value_location
        )
        {
            std::scoped_lock lock(_live_input_bindings_mutex);
            auto const key = member_ordinal.has_value()
                ? concrete_key(logical_node_id, *member_ordinal)
                : std::string(logical_node_id);
            auto& slots = ensure_live_input_slots_locked(key, input_ordinal);
            for (auto& slot : slots) {
                if (slot == previous_location) {
                    slot = value_location;
                    break;
                }
            }
            Sample value = member_ordinal.has_value()
                ? live_input_value_or_locked(logical_node_id, *member_ordinal, input_ordinal, *value_location)
                : live_input_value_or_locked(logical_node_id, input_ordinal, *value_location);
            *value_location = value;
        }

        void unregister_live_input_value(
            std::string_view logical_node_id,
            std::optional<size_t> member_ordinal,
            size_t input_ordinal,
            Sample* value_location
        )
        {
            std::scoped_lock lock(_live_input_bindings_mutex);
            auto const key = member_ordinal.has_value()
                ? concrete_key(logical_node_id, *member_ordinal)
                : std::string(logical_node_id);
            auto it = _live_inputs.find(key);
            if (it == _live_inputs.end() || it->second.size() <= input_ordinal) {
                return;
            }

            auto& slots = it->second[input_ordinal];
            slots.erase(std::remove(slots.begin(), slots.end(), value_location), slots.end());
        }

        void set_live_input_value(
            std::string_view logical_node_id,
            size_t input_ordinal,
            Sample value
        )
        {
            std::scoped_lock lock(_live_input_bindings_mutex);
            ensure_live_input_value_locked(logical_node_id, input_ordinal) = value;
            if (auto it = _live_inputs.find(std::string(logical_node_id));
                it != _live_inputs.end() && it->second.size() > input_ordinal) {
                for (auto* slot : it->second[input_ordinal]) {
                    if (slot) {
                        *slot = value;
                    }
                }
            }
            std::string const prefix = concrete_key_prefix(logical_node_id);
            for (auto& [key, slots_by_input] : _live_inputs) {
                if (!key.starts_with(prefix) || slots_by_input.size() <= input_ordinal) {
                    continue;
                }
                auto const input_override_key = key + "\x1finput:" + std::to_string(input_ordinal);
                if (_concrete_live_input_overrides.contains(input_override_key)) {
                    continue;
                }
                for (auto* slot : slots_by_input[input_ordinal]) {
                    if (slot) {
                        *slot = value;
                    }
                }
            }
        }

        void set_live_input_value(
            std::string_view logical_node_id,
            size_t member_ordinal,
            size_t input_ordinal,
            Sample value
        )
        {
            std::scoped_lock lock(_live_input_bindings_mutex);
            auto const key = concrete_key(logical_node_id, member_ordinal);
            _concrete_live_input_overrides.insert(concrete_override_key(logical_node_id, member_ordinal, input_ordinal));
            ensure_live_input_value_locked(key, input_ordinal) = value;
            auto& slots = ensure_live_input_slots_locked(key, input_ordinal);
            for (auto* slot : slots) {
                if (slot) {
                    *slot = value;
                }
            }
        }

        void clear_live_input_value_override(
            std::string_view logical_node_id,
            size_t member_ordinal,
            size_t input_ordinal
        )
        {
            std::scoped_lock lock(_live_input_bindings_mutex);
            _concrete_live_input_overrides.erase(concrete_override_key(logical_node_id, member_ordinal, input_ordinal));

            auto const key = concrete_key(logical_node_id, member_ordinal);
            auto const logical_value = live_input_value_or_locked(logical_node_id, input_ordinal, Sample {0.0f});
            ensure_live_input_value_locked(key, input_ordinal) = logical_value;
            auto& slots = ensure_live_input_slots_locked(key, input_ordinal);
            for (auto* slot : slots) {
                if (slot) {
                    *slot = logical_value;
                }
            }
        }

        bool has_live_input_value_override(
            std::string_view logical_node_id,
            size_t member_ordinal,
            size_t input_ordinal
        ) const
        {
            std::scoped_lock lock(_live_input_bindings_mutex);
            return _concrete_live_input_overrides.contains(
                concrete_override_key(logical_node_id, member_ordinal, input_ordinal)
            );
        }

        Sample live_input_value_or(
            std::string_view logical_node_id,
            size_t input_ordinal,
            Sample fallback
        ) const
        {
            std::scoped_lock lock(_live_input_bindings_mutex);
            return live_input_value_or_locked(logical_node_id, input_ordinal, fallback);
        }

        Sample live_input_value_or(
            std::string_view logical_node_id,
            size_t member_ordinal,
            size_t input_ordinal,
            Sample fallback
        ) const
        {
            std::scoped_lock lock(_live_input_bindings_mutex);
            return live_input_value_or_locked(logical_node_id, member_ordinal, input_ordinal, fallback);
        }

    private:
        Sample live_input_value_or_locked(
            std::string_view logical_node_id,
            size_t input_ordinal,
            Sample fallback
        ) const
        {
            auto it = _live_input_values.find(std::string(logical_node_id));
            if (it == _live_input_values.end() || it->second.size() <= input_ordinal) {
                return fallback;
            }
            return it->second[input_ordinal];
        }

        Sample live_input_value_or_locked(
            std::string_view logical_node_id,
            size_t member_ordinal,
            size_t input_ordinal,
            Sample fallback
        ) const
        {
            auto const override = concrete_override_key(logical_node_id, member_ordinal, input_ordinal);
            auto const key = concrete_key(logical_node_id, member_ordinal);
            if (_concrete_live_input_overrides.contains(override)) {
                auto it = _live_input_values.find(key);
                if (it != _live_input_values.end() && it->second.size() > input_ordinal) {
                    return it->second[input_ordinal];
                }
            }
            return live_input_value_or_locked(logical_node_id, input_ordinal, fallback);
        }
    };
}
