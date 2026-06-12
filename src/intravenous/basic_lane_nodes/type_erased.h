#pragma once

#include <intravenous/lane_node/generate.h>
#include <intravenous/runtime/lane_graph.h>

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
        void (*_tick_block_compiled_fn)(void*, CompiledLaneTickContext<TypeErasedLaneNode>&) = nullptr;
        void (*_tick_block_realtime_fn)(void*, RealtimeLaneTickContext<TypeErasedLaneNode>&) = nullptr;
        std::vector<CompiledSupportRange> (*_compiled_support_ranges_fn)(void*, CompiledSupportContext<TypeErasedLaneNode>&) = nullptr;
        void (*_on_inputs_changed_fn)(void*, InputsChangedContext<TypeErasedLaneNode>&) = nullptr;
        bool _supports_tick_block_compiled = false;
        bool _supports_tick_block_realtime = false;
        bool _supports_compiled_support_ranges = false;
        bool _has_on_inputs_changed = false;

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
            if constexpr (lane_node_details::has_tick_block_compiled<LaneNode>) {
                static_assert(
                    lane_node_details::has_compiled_support_ranges<LaneNode>,
                    "compiled-capable lane nodes must define compiled_support_ranges(ctx)");
                _tick_block_compiled_fn = +[](void* node_ptr, CompiledLaneTickContext<TypeErasedLaneNode>& erased_ctx) {
                    CompiledLaneTickContext<LaneNode> ctx(erased_ctx.untyped());
                    do_tick_block_compiled(*static_cast<LaneNode*>(node_ptr), ctx);
                };
                _supports_tick_block_compiled = true;
            }
            if constexpr (lane_node_details::has_tick_block_realtime<LaneNode> || lane_node_details::has_tick_block_compiled<LaneNode>) {
                _tick_block_realtime_fn = +[](void* node_ptr, RealtimeLaneTickContext<TypeErasedLaneNode>& erased_ctx) {
                    RealtimeLaneTickContext<LaneNode> ctx(erased_ctx.untyped());
                    do_tick_block_realtime(*static_cast<LaneNode*>(node_ptr), ctx);
                };
                _supports_tick_block_realtime = true;
            }
            if constexpr (lane_node_details::has_compiled_support_ranges<LaneNode>) {
                _compiled_support_ranges_fn = +[](void* node_ptr, CompiledSupportContext<TypeErasedLaneNode>& erased_ctx) {
                    CompiledSupportContext<LaneNode> ctx(erased_ctx.untyped());
                    return get_compiled_support_ranges(*static_cast<LaneNode*>(node_ptr), ctx);
                };
                _supports_compiled_support_ranges = true;
            }
            if constexpr (lane_node_details::has_on_inputs_changed<LaneNode>) {
                _on_inputs_changed_fn = +[](void* node_ptr, InputsChangedContext<TypeErasedLaneNode>& erased_ctx) {
                    InputsChangedContext<LaneNode> ctx(erased_ctx.untyped());
                    do_on_inputs_changed(*static_cast<LaneNode*>(node_ptr), ctx);
                };
                _has_on_inputs_changed = true;
            }
        }

        std::vector<CompiledSampleLaneInputConfig> const& compiled_sample_inputs() const { return _compiled_sample_inputs; }
        std::vector<CompiledEventLaneInputConfig> const& compiled_event_inputs() const { return _compiled_event_inputs; }
        std::vector<RealtimeSampleLaneInputConfig> const& realtime_sample_inputs() const { return _realtime_sample_inputs; }
        std::vector<RealtimeEventLaneInputConfig> const& realtime_event_inputs() const { return _realtime_event_inputs; }
        LaneOutputConfig const& output() const { return _output; }
        char const* type_name() const { return _type_name; }
        bool supports_tick_block_compiled() const { return _supports_tick_block_compiled; }
        bool supports_tick_block_realtime() const { return _supports_tick_block_realtime; }
        bool supports_compiled_support_ranges() const { return _supports_compiled_support_ranges; }
        bool has_on_inputs_changed() const { return _has_on_inputs_changed; }

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

        void tick_block_compiled(CompiledLaneTickContext<TypeErasedLaneNode>& ctx) const
        {
            if (_tick_block_compiled_fn) {
                _tick_block_compiled_fn(_node.get(), ctx);
            }
        }

        void tick_block_realtime(RealtimeLaneTickContext<TypeErasedLaneNode>& ctx) const
        {
            if (_tick_block_realtime_fn) {
                _tick_block_realtime_fn(_node.get(), ctx);
            }
        }

        std::vector<CompiledSupportRange> compiled_support_ranges(
            CompiledSupportContext<TypeErasedLaneNode>& ctx) const
        {
            if (_compiled_support_ranges_fn) {
                return _compiled_support_ranges_fn(_node.get(), ctx);
            }
            return {};
        }

        void on_inputs_changed(InputsChangedContext<TypeErasedLaneNode>& ctx) const
        {
            if (_on_inputs_changed_fn) {
                _on_inputs_changed_fn(_node.get(), ctx);
            }
        }
    };
}
