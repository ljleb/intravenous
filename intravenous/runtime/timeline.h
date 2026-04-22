#pragma once

#include "sample.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
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
        std::unordered_map<std::string, std::vector<Sample*>> _live_inputs;

        Sample*& ensure_live_input_slot_locked(
            std::string_view logical_node_id,
            size_t input_ordinal
        )
        {
            auto& slots = _live_inputs[std::string(logical_node_id)];
            if (slots.size() <= input_ordinal) {
                slots.resize(input_ordinal + 1);
            }
            return slots[input_ordinal];
        }

        friend class TimelineAugmentation;

    public:
        TimelineAugmentation begin_augmentation()
        {
            return TimelineAugmentation(*this);
        }

        void register_live_input_value(
            std::string_view logical_node_id,
            size_t input_ordinal,
            Sample* value_location
        )
        {
            std::scoped_lock lock(_live_input_bindings_mutex);
            auto& slot = ensure_live_input_slot_locked(logical_node_id, input_ordinal);
            slot = value_location;
        }

        void move_live_input_value(
            std::string_view logical_node_id,
            size_t input_ordinal,
            Sample* previous_location,
            Sample* value_location
        )
        {
            std::scoped_lock lock(_live_input_bindings_mutex);
            auto& slot = ensure_live_input_slot_locked(logical_node_id, input_ordinal);
            if (slot == previous_location) {
                slot = value_location;
            } else {
                slot = value_location;
            }
        }

        void unregister_live_input_value(
            std::string_view logical_node_id,
            size_t input_ordinal,
            Sample* value_location
        )
        {
            std::scoped_lock lock(_live_input_bindings_mutex);
            auto it = _live_inputs.find(std::string(logical_node_id));
            if (it == _live_inputs.end() || it->second.size() <= input_ordinal) {
                return;
            }

            auto& slot = it->second[input_ordinal];
            if (slot == value_location) {
                slot = nullptr;
            }
        }

        void set_live_input_value(
            std::string_view logical_node_id,
            size_t input_ordinal,
            Sample value
        )
        {
            std::scoped_lock lock(_live_input_bindings_mutex);
            auto& slot = ensure_live_input_slot_locked(logical_node_id, input_ordinal);
            if (slot) {
                *slot = value;
            }
        }
    };
}
