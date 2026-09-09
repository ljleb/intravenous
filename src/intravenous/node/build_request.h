#pragma once

// The module-side node boundary.  Templates here provide the genuinely
// type-specific work: construct a node value, emit its compiler record, and
// enumerate its declared properties.  The builder library owns the resulting
// graph description, dynamic containers, configuration storage, and
// validation.

#include <intravenous/basic_nodes/constant.h>
#include <intravenous/node/compiler_record.h>
#include <intravenous/node/lifecycle.h>

#include <cstddef>
#include <concepts>
#include <memory>
#include <optional>
#include <type_traits>

namespace iv {
struct ReflectedNodeDescription;

namespace details {

class NodeDescriptionBuilder;

class NodeDescriptionSink {
    void* _description = nullptr;

    ReflectedNodeDescription& description() const;

    explicit NodeDescriptionSink(void* description) noexcept
        : _description(description)
    {}

    friend class NodeDescriptionBuilder;

public:
    void add_sample_input(InputConfig const&) const;
    void add_sample_output(OutputConfig const&) const;
    void add_event_input(EventInputConfig const&) const;
    void add_event_output(EventOutputConfig const&) const;
    void set_internal_latency(std::size_t) const;
    void set_maximum_block_size(std::size_t) const;
    void set_default_ttl(std::optional<std::size_t>) const;
    void set_block_skippable(bool) const;
    void set_static_sample_value(std::optional<Sample>) const;
};

struct NodeBuildRequest {
    NodeCompilerRecord const* compiler_record = nullptr;
    void const* config = nullptr;
    std::size_t config_size = 0;
    std::size_t config_alignment = 1;
    void (*describe)(void const*, NodeDescriptionSink&) = nullptr;
};

template<class Node>
std::size_t declare_node(
    void const* node_data,
    NodeStateStructure const* state_structure,
    NodeLayoutBuilder& builder)
{
    auto const& node = *static_cast<Node const*>(node_data);
    DeclarationContext<Node> ctx(builder, node);
    if (state_structure) {
        details::override_node_state_structure(
            builder, ctx.node_index(), *state_structure);
    }
    if constexpr (has_declare<Node>) {
        node.declare(ctx);
    }
    return ctx.node_index();
}

template<class Node>
IV_FORCEINLINE void tick_node_block(
    void const* node_data,
    ReflectedNodeTickContext const& ctx,
    std::size_t index,
    std::size_t block_size)
{
    auto const& node = *static_cast<Node const*>(node_data);
    do_tick_block(node, TickBlockContext<Node> {
        TickContext<Node> {
            .inputs = ctx.inputs,
            .outputs = ctx.outputs,
            .event_inputs = ctx.event_inputs,
            .event_outputs = ctx.event_outputs,
            .sample_rate = ctx.sample_rate,
            .scc_feedback_latency = ctx.scc_feedback_latency,
            .buffer = ctx.state,
        },
        index,
        block_size,
    });
}

template<class Node>
IV_FORCEINLINE void skip_node_block(
    void const* node_data,
    ReflectedNodeTickContext const& ctx,
    std::size_t index,
    std::size_t block_size)
{
    auto const& node = *static_cast<Node const*>(node_data);
    do_skip_block(node, SkipBlockContext<Node> {
        TickContext<Node> {
            .inputs = ctx.inputs,
            .outputs = ctx.outputs,
            .event_inputs = ctx.event_inputs,
            .event_outputs = ctx.event_outputs,
            .sample_rate = ctx.sample_rate,
            .scc_feedback_latency = ctx.scc_feedback_latency,
            .buffer = ctx.state,
        },
        index,
        block_size,
    });
}

template<class Node>
constexpr ReflectedNodeRuntimeOperations node_runtime_operations()
{
    return {
        .node_data = nullptr,
        .state_structure = nullptr,
        .declare_node = &declare_node<Node>,
        .tick_block = &tick_node_block<Node>,
        .skip_block = &skip_node_block<Node>,
    };
}

template<class Node>
consteval std::size_t node_state_size()
{
    using State = typename NodeState<Node>::Type;
    if constexpr (std::is_void_v<State>) {
        return 0;
    } else {
        return sizeof(State);
    }
}

template<class Node>
consteval std::size_t node_state_alignment()
{
    using State = typename NodeState<Node>::Type;
    if constexpr (std::is_void_v<State>) {
        return 1;
    } else {
        return alignof(State);
    }
}

#if defined(__APPLE__)
#define IV_NODE_COMPILER_RECORD_ATTR __attribute__((used, section("__DATA,__iv_node_types")))
#elif defined(_WIN32)
#define IV_NODE_COMPILER_RECORD_ATTR __declspec(selectany)
#else
#define IV_NODE_COMPILER_RECORD_ATTR __attribute__((used, section("iv_node_types")))
#endif

template<class Node>
IV_NODE_COMPILER_RECORD_ATTR inline const NodeCompilerRecord
    node_compiler_record {
        .code_key = node_code_key_v<Node>,
        .runtime = node_runtime_operations<Node>(),
        .type_name = clang_type_name<Node>().data(),
        .type_name_size = clang_type_name<Node>().size(),
        .state_size = node_state_size<Node>(),
        .state_alignment = node_state_alignment<Node>(),
    };

#undef IV_NODE_COMPILER_RECORD_ATTR

template<class Node>
void describe_node(void const* node_data, NodeDescriptionSink& sink)
{
    auto const& node = *static_cast<Node const*>(node_data);
    for (auto const& input : get_inputs(node)) {
        sink.add_sample_input(input);
    }
    for (auto const& output : get_outputs(node)) {
        sink.add_sample_output(output);
    }
    for (auto const& input : get_event_inputs(node)) {
        sink.add_event_input(input);
    }
    for (auto const& output : get_event_outputs(node)) {
        sink.add_event_output(output);
    }
    sink.set_internal_latency(get_internal_latency(node));
    sink.set_maximum_block_size(get_max_block_size(node));
    sink.set_default_ttl(get_ttl_samples(node));
    sink.set_block_skippable(get_can_skip_block(node));
    if constexpr (std::same_as<std::remove_cvref_t<Node>, Constant>) {
        sink.set_static_sample_value(node._value);
    }
}

template<class Node>
NodeBuildRequest make_node_build_request(Node const& node)
{
    using Value = std::remove_cvref_t<Node>;
    // Emit the build-local record in the LLVM module.  The builder consumes
    // it synchronously and retains only copied data and the NodeCodeKey.
    auto const* record = &node_compiler_record<Value>;
    return {
        .compiler_record = record,
        .config = std::addressof(node),
        .config_size = sizeof(Value),
        .config_alignment = alignof(Value),
        .describe = &describe_node<Value>,
    };
}

} // namespace details
} // namespace iv
