#pragma once

#include "lane_node/generate.h"
#include "runtime/lane_graph.h"

#include <memory>
#include <typeinfo>
#include <utility>
#include <vector>

namespace iv {
    class TypeErasedLaneNode {
        using NodeStoragePtr = std::unique_ptr<void, void(*)(void*)>;

        NodeStoragePtr _node { nullptr, +[](void*) {} };
        std::vector<CompiledSampleLaneInputConfig> _compiled_sample_inputs;
        std::vector<CompiledEventLaneInputConfig> _compiled_event_inputs;
        std::vector<RealtimeSampleLaneInputConfig> _realtime_sample_inputs;
        std::vector<RealtimeEventLaneInputConfig> _realtime_event_inputs;
        LaneOutputConfig _output {};
        char const* _type_name = "<unknown>";
        std::type_info const* _type_info = &typeid(void);
        void const* (*_const_ptr_fn)(void const*) = +[](void const*) -> void const* { return nullptr; };
        void* (*_ptr_fn)(void*) = +[](void*) -> void* { return nullptr; };
        void (*_generate_fn)(void*, TimelineGenerateContext<TypeErasedLaneNode>&) = nullptr;

    public:
        TypeErasedLaneNode() = default;
        TypeErasedLaneNode(TypeErasedLaneNode const&) = delete;
        TypeErasedLaneNode& operator=(TypeErasedLaneNode const&) = delete;
        TypeErasedLaneNode(TypeErasedLaneNode&&) noexcept = default;
        TypeErasedLaneNode& operator=(TypeErasedLaneNode&&) noexcept = default;

        template<typename LaneNode>
        /*implicit*/ TypeErasedLaneNode(LaneNode node)
        {
            auto const compiled_sample_inputs = get_compiled_sample_lane_inputs(node);
            auto const compiled_event_inputs = get_compiled_event_lane_inputs(node);
            auto const realtime_sample_inputs = get_realtime_sample_lane_inputs(node);
            auto const realtime_event_inputs = get_realtime_event_lane_inputs(node);
            _compiled_sample_inputs.assign(compiled_sample_inputs.begin(), compiled_sample_inputs.end());
            _compiled_event_inputs.assign(compiled_event_inputs.begin(), compiled_event_inputs.end());
            _realtime_sample_inputs.assign(realtime_sample_inputs.begin(), realtime_sample_inputs.end());
            _realtime_event_inputs.assign(realtime_event_inputs.begin(), realtime_event_inputs.end());
            _output = normalize_lane_output(get_lane_output(node));
            _type_name = typeid(LaneNode).name();
            _type_info = &typeid(LaneNode);
            _node = NodeStoragePtr(
                new LaneNode(std::move(node)),
                +[](void* ptr) { delete static_cast<LaneNode*>(ptr); }
            );
            _const_ptr_fn = +[](void const* ptr) -> void const* { return ptr; };
            _ptr_fn = +[](void* ptr) -> void* { return ptr; };
            _generate_fn = [](void* node_ptr, TimelineGenerateContext<TypeErasedLaneNode>& erased_ctx) {
                TimelineGenerateContext<LaneNode> ctx(erased_ctx);
                do_timeline_generate(*static_cast<LaneNode*>(node_ptr), ctx);
            };
        }

        std::vector<CompiledSampleLaneInputConfig> const& compiled_sample_inputs() const
        {
            return _compiled_sample_inputs;
        }

        std::vector<CompiledEventLaneInputConfig> const& compiled_event_inputs() const
        {
            return _compiled_event_inputs;
        }

        std::vector<RealtimeSampleLaneInputConfig> const& realtime_sample_inputs() const
        {
            return _realtime_sample_inputs;
        }

        std::vector<RealtimeEventLaneInputConfig> const& realtime_event_inputs() const
        {
            return _realtime_event_inputs;
        }

        LaneOutputConfig const& output() const
        {
            return _output;
        }

        char const* type_name() const
        {
            return _type_name;
        }

        template<typename LaneNode>
        LaneNode const* try_as() const
        {
            if (*_type_info != typeid(LaneNode)) {
                return nullptr;
            }
            return static_cast<LaneNode const*>(_const_ptr_fn(_node.get()));
        }

        template<typename LaneNode>
        LaneNode* try_as()
        {
            if (*_type_info != typeid(LaneNode)) {
                return nullptr;
            }
            return static_cast<LaneNode*>(_ptr_fn(_node.get()));
        }

        void generate(TimelineGenerateContext<TypeErasedLaneNode>& ctx)
        {
            _generate_fn(_node.get(), ctx);
        }
    };
}
