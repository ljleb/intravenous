#pragma once

// The module-side node boundary.  Templates here provide the genuinely
// type-specific work: construct a node value, emit its compiler record, and
// enumerate its declared properties.  The builder library owns the resulting
// graph description, dynamic containers, configuration storage, and
// validation.

#include <intravenous/basic_nodes/constant.h>
#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/node/compiled_port_context.h>
#include <intravenous/node/compiler_record.h>
#include <intravenous/node/lifecycle.h>

#include <array>
#include <cstddef>
#include <concepts>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

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
    void add_input(InputConfig const&) const;
    void add_output(OutputConfig const&) const;
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
    NodeStateStructures const* state_structures,
    NodeLayoutBuilder& builder)
{
    auto const& node = *static_cast<Node const*>(node_data);
    DeclarationContext<Node> ctx(builder, node);
    if (state_structures) {
        details::override_node_state_structures(
            builder, ctx.node_index(), *state_structures);
    }
    if constexpr (has_declare<Node>) {
        node.declare(ctx);
    }
    return ctx.node_index();
}

template<typename Node>
consteval std::size_t reflected_sample_input_count()
{
    if constexpr (has_inputs<Node> && has_constexpr_port_configs<Node>) {
        return count_sample_ports(Node::inputs());
    } else {
        return 0;
    }
}

template<typename Node>
consteval std::size_t reflected_sample_output_count()
{
    if constexpr (has_outputs<Node> && has_constexpr_port_configs<Node>) {
        return count_sample_ports(Node::outputs());
    } else {
        return 0;
    }
}

template<typename Node>
inline constexpr std::size_t reflected_sample_input_count_v =
    reflected_sample_input_count<Node>();

template<typename Node>
inline constexpr std::size_t reflected_sample_output_count_v =
    reflected_sample_output_count<Node>();

template<typename Node>
consteval std::size_t reflected_event_input_count()
{
    if constexpr (has_inputs<Node> && has_constexpr_port_configs<Node>) {
        return count_event_ports(Node::inputs());
    } else {
        return 0;
    }
}

template<typename Node>
consteval std::size_t reflected_event_output_count()
{
    if constexpr (has_outputs<Node> && has_constexpr_port_configs<Node>) {
        return count_event_ports(Node::outputs());
    } else {
        return 0;
    }
}

template<typename Node>
inline constexpr std::size_t reflected_event_input_count_v =
    reflected_event_input_count<Node>();

template<typename Node>
inline constexpr std::size_t reflected_event_output_count_v =
    reflected_event_output_count<Node>();

IV_FORCEINLINE SamplePortStorageView reflected_sample_storage_view(
    std::byte* storage_base,
    ReflectedSamplePortStorageBinding const& binding)
{
    auto const sample_count = sample_storage_size(
        binding.channel_layout, binding.frame_capacity);
    auto* samples = reinterpret_cast<Sample*>(
        storage_base + binding.storage_offset);
    return SamplePortStorageView{
        std::span<Sample>{samples, sample_count},
        binding.storage_latency,
        binding.channel_layout,
        binding.frame_capacity,
    };
}

IV_FORCEINLINE InputPort reflected_sample_input_port(
    std::byte* storage_base,
    ReflectedSampleInputPortBinding const& binding,
    SampleIndex index)
{
    return InputPort{
        reflected_sample_storage_view(storage_base, binding.storage),
        binding.history,
        binding.read_latency,
        index,
    };
}

IV_FORCEINLINE OutputPort reflected_sample_output_port(
    std::byte* storage_base,
    ReflectedSampleOutputPortBinding const& binding,
    SampleIndex index)
{
    return OutputPort{
        reflected_sample_storage_view(storage_base, binding.storage),
        binding.history,
        index,
    };
}

template<typename Node, std::size_t... I>
IV_FORCEINLINE auto reflected_sample_inputs(
    ReflectedNodeTickContext const& ctx,
    SampleIndex index,
    std::index_sequence<I...>)
{
    return std::array<InputPort, sizeof...(I)>{
        reflected_sample_input_port(
            ctx.sample_storage_base,
            ctx.sample_input_bindings.pointer[I],
            index)...
    };
}

template<typename Node, std::size_t... I>
IV_FORCEINLINE auto reflected_sample_outputs(
    ReflectedNodeTickContext const& ctx,
    SampleIndex index,
    std::index_sequence<I...>)
{
    return std::array<OutputPort, sizeof...(I)>{
        reflected_sample_output_port(
            ctx.sample_storage_base,
            ctx.sample_output_bindings.pointer[I],
            index)...
    };
}

template<typename Node, typename Fn>
IV_FORCEINLINE void with_reflected_sample_ports(
    ReflectedNodeTickContext const& ctx,
    SampleIndex index,
    Fn&& fn)
{
    if constexpr (!has_constexpr_port_configs<Node>) {
        std::forward<Fn>(fn)(
            static_cast<std::span<InputPort>>(ctx.inputs),
            static_cast<std::span<OutputPort>>(ctx.outputs));
        return;
    } else {
        if (ctx.sample_storage_base == nullptr) {
            std::forward<Fn>(fn)(
                static_cast<std::span<InputPort>>(ctx.inputs),
                static_cast<std::span<OutputPort>>(ctx.outputs));
            return;
        }

        constexpr auto input_count = reflected_sample_input_count_v<Node>;
        constexpr auto output_count = reflected_sample_output_count_v<Node>;
        IV_ASSERT(
            ctx.sample_input_bindings.size() == input_count,
            "reflected sample input binding count does not match node declaration");
        IV_ASSERT(
            ctx.sample_output_bindings.size() == output_count,
            "reflected sample output binding count does not match node declaration");

        auto inputs = reflected_sample_inputs<Node>(
            ctx, index, std::make_index_sequence<input_count>{});
        auto outputs = reflected_sample_outputs<Node>(
            ctx, index, std::make_index_sequence<output_count>{});
        std::forward<Fn>(fn)(
            std::span<InputPort>{inputs},
            std::span<OutputPort>{outputs});
    }
}

template<std::size_t N>
struct ReflectedEventInputPorts {
    std::array<EventSharedPortData, N> shared{};
    std::array<EventInputPort, N> ports{};
};

template<std::size_t N>
struct ReflectedEventOutputPorts {
    std::array<EventSharedPortData, N> shared{};
    std::array<EventOutputPort, N> ports{};
    std::array<std::size_t*, N> counts{};
};

IV_FORCEINLINE std::span<TimedEvent> reflected_event_buffer(
    std::byte* storage_base,
    ReflectedEventPortStorageBinding const& binding)
{
    auto* events = reinterpret_cast<TimedEvent*>(
        storage_base + binding.events_offset);
    return {events, binding.event_capacity};
}

IV_FORCEINLINE std::size_t* reflected_event_count(
    std::byte* storage_base,
    ReflectedEventPortStorageBinding const& binding)
{
    return reinterpret_cast<std::size_t*>(
        storage_base + binding.count_offset);
}

template<std::size_t N, std::size_t... I>
IV_FORCEINLINE void initialize_reflected_event_inputs(
    ReflectedEventInputPorts<N>& result,
    ReflectedNodeTickContext const& ctx,
    std::index_sequence<I...>)
{
    static_assert(N == sizeof...(I));
    (([&] {
        auto const& binding = ctx.event_input_bindings.pointer[I].storage;
        auto* count = reflected_event_count(ctx.event_storage_base, binding);
        result.shared[I] = EventSharedPortData{
            reflected_event_buffer(ctx.event_storage_base, binding),
            0,
            *count,
            binding.type,
        };
        result.ports[I] = EventInputPort{result.shared[I]};
    }()), ...);
}

template<std::size_t N, std::size_t... I>
IV_FORCEINLINE void initialize_reflected_event_outputs(
    ReflectedEventOutputPorts<N>& result,
    ReflectedNodeTickContext const& ctx,
    SampleIndex index,
    std::size_t block_size,
    std::index_sequence<I...>)
{
    static_assert(N == sizeof...(I));
    (([&] {
        auto const& binding = ctx.event_output_bindings.pointer[I];
        auto* count = reflected_event_count(
            ctx.event_storage_base, binding.storage);
        auto const initial_write = binding.append_existing ? *count : 0;
        if (!binding.append_existing) {
            *count = 0;
        }
        result.counts[I] = count;
        result.shared[I] = EventSharedPortData{
            reflected_event_buffer(ctx.event_storage_base, binding.storage),
            0,
            initial_write,
            binding.storage.type,
        };
        auto* overflow_count = reinterpret_cast<std::uint64_t*>(
            ctx.event_storage_base + binding.overflow_count_offset);
        result.ports[I] = EventOutputPort{
            result.shared[I],
            binding.source_type,
            binding.history,
            binding.latency,
            overflow_count,
        };
        result.ports[I].begin_block(index, block_size);
    }()), ...);
}

template<std::size_t N>
IV_FORCEINLINE void commit_reflected_event_outputs(
    ReflectedEventOutputPorts<N>& outputs)
{
    for (std::size_t i = 0; i < N; ++i) {
        *outputs.counts[i] = outputs.shared[i].write_index;
        outputs.ports[i].end_block();
    }
}

template<typename Node, typename Fn>
IV_FORCEINLINE void with_reflected_event_ports(
    ReflectedNodeTickContext const& ctx,
    SampleIndex index,
    std::size_t block_size,
    Fn&& fn)
{
    if constexpr (!has_constexpr_port_configs<Node>) {
        std::forward<Fn>(fn)(
            static_cast<std::span<EventInputPort>>(ctx.event_inputs),
            static_cast<std::span<EventOutputPort>>(ctx.event_outputs));
        return;
    } else {
        if (ctx.event_storage_base == nullptr) {
            std::forward<Fn>(fn)(
                static_cast<std::span<EventInputPort>>(ctx.event_inputs),
                static_cast<std::span<EventOutputPort>>(ctx.event_outputs));
            return;
        }

        constexpr auto input_count = reflected_event_input_count_v<Node>;
        constexpr auto output_count = reflected_event_output_count_v<Node>;
        IV_ASSERT(
            ctx.event_input_bindings.size() == input_count,
            "reflected event input binding count does not match node declaration");
        IV_ASSERT(
            ctx.event_output_bindings.size() == output_count,
            "reflected event output binding count does not match node declaration");

        ReflectedEventInputPorts<input_count> inputs;
        ReflectedEventOutputPorts<output_count> outputs;
        initialize_reflected_event_inputs(
            inputs, ctx, std::make_index_sequence<input_count>{});
        initialize_reflected_event_outputs(
            outputs, ctx, index, block_size,
            std::make_index_sequence<output_count>{});
        std::forward<Fn>(fn)(
            std::span<EventInputPort>{inputs.ports},
            std::span<EventOutputPort>{outputs.ports});
        commit_reflected_event_outputs(outputs);
    }
}

template<class Node>
IV_FORCEINLINE void tick_node_block(
    void const* node_data,
    ReflectedNodeTickContext const& ctx,
    std::size_t index,
    std::size_t block_size)
{
    auto const& node = *static_cast<Node const*>(node_data);
    with_reflected_sample_ports<Node>(
        ctx,
        static_cast<SampleIndex>(index),
        [&](std::span<InputPort> inputs, std::span<OutputPort> outputs) {
            with_reflected_event_ports<Node>(
                ctx,
                static_cast<SampleIndex>(index),
                block_size,
                [&](std::span<EventInputPort> event_inputs,
                    std::span<EventOutputPort> event_outputs) {
                    do_tick_block(node, TickBlockContext<Node> {
                        TickContext<Node> {
                            .inputs = inputs,
                            .outputs = outputs,
                            .event_inputs = event_inputs,
                            .event_outputs = event_outputs,
                            .compiled_inputs = ctx.compiled_inputs,
                            .compiled_event_inputs = ctx.compiled_event_inputs,
                            .compiled_state_storage = ctx.compiled_state,
                            .sample_rate = ctx.sample_rate,
                            .scc_feedback_latency = ctx.scc_feedback_latency,
                            .buffer = ctx.state,
                        },
                        static_cast<SampleIndex>(index),
                        block_size,
                    });
                });
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
    with_reflected_sample_ports<Node>(
        ctx,
        static_cast<SampleIndex>(index),
        [&](std::span<InputPort> inputs, std::span<OutputPort> outputs) {
            with_reflected_event_ports<Node>(
                ctx,
                static_cast<SampleIndex>(index),
                block_size,
                [&](std::span<EventInputPort> event_inputs,
                    std::span<EventOutputPort> event_outputs) {
                    do_skip_block(node, SkipBlockContext<Node> {
                        TickContext<Node> {
                            .inputs = inputs,
                            .outputs = outputs,
                            .event_inputs = event_inputs,
                            .event_outputs = event_outputs,
                            .compiled_inputs = ctx.compiled_inputs,
                            .compiled_event_inputs = ctx.compiled_event_inputs,
                            .compiled_state_storage = ctx.compiled_state,
                            .sample_rate = ctx.sample_rate,
                            .scc_feedback_latency = ctx.scc_feedback_latency,
                            .buffer = ctx.state,
                        },
                        static_cast<SampleIndex>(index),
                        block_size,
                    });
                });
        });
}

template<class Node>
IV_FORCEINLINE void access_node_block_batched(
    void const* node_data, void* opaque_context)
{
    auto const& node = *static_cast<Node const*>(node_data);
    auto& context = *static_cast<AccessBlockBatchContext<Node>*>(opaque_context);
    do_access_block_batched(node, context);
}

template<class Node>
IV_FORCEINLINE void propagate_node_block_access_batched(
    void const* node_data, void* opaque_context)
{
    auto const& node = *static_cast<Node const*>(node_data);
    auto& context =
        *static_cast<PropagateBlockAccessBatchContext<Node>*>(opaque_context);
    do_propagate_block_access_batched<Node>()(node, context);
}

template<class Node>
consteval auto node_access_block_batched_operation()
{
    if constexpr (details::declares_compiled_outputs_v<Node>) {
        static_assert(details::has_valid_access_block_callback_v<Node>,
            "compiled-output node has no valid access_block/access_block_batch implementation");
        return &access_node_block_batched<Node>;
    } else {
        return static_cast<void (*)(void const*, void*)>(nullptr);
    }
}

template<class Node>
consteval auto node_propagate_block_access_batched_operation()
{
    if constexpr (details::declares_compiled_outputs_v<Node>
        && details::declares_compiled_inputs_v<Node>) {
        return &propagate_node_block_access_batched<Node>;
    } else {
        return static_cast<void (*)(void const*, void*)>(nullptr);
    }
}

template<class Node>
constexpr NodeCompilerOperations node_compiler_operations()
{
    return {
        .declare_node = &declare_node<Node>,
        .tick_block = &tick_node_block<Node>,
        .skip_block = &skip_node_block<Node>,
        .access_block_batched = node_access_block_batched_operation<Node>(),
        .propagate_block_access_batched =
            node_propagate_block_access_batched_operation<Node>(),
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

template<class Node>
consteval std::size_t node_compiled_state_size()
{
    using CompiledState = typename NodeCompiledState<Node>::Type;
    if constexpr (std::is_void_v<CompiledState>) {
        return 0;
    } else {
        return sizeof(CompiledState);
    }
}

template<class Node>
consteval std::size_t node_compiled_state_alignment()
{
    using CompiledState = typename NodeCompiledState<Node>::Type;
    if constexpr (std::is_void_v<CompiledState>) {
        return 1;
    } else {
        return alignof(CompiledState);
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
        .operations = node_compiler_operations<Node>(),
        .type_name = clang_type_name<Node>().data(),
        .type_name_size = clang_type_name<Node>().size(),
        .state_size = node_state_size<Node>(),
        .state_alignment = node_state_alignment<Node>(),
        .compiled_state_size = node_compiled_state_size<Node>(),
        .compiled_state_alignment = node_compiled_state_alignment<Node>(),
    };

#undef IV_NODE_COMPILER_RECORD_ATTR

template<class Node>
void describe_node(void const* node_data, NodeDescriptionSink& sink)
{
    auto const& node = *static_cast<Node const*>(node_data);
    for (InputConfig const& input : get_declared_inputs(node)) {
        sink.add_input(input);
    }
    for (OutputConfig const& output : get_declared_outputs(node)) {
        sink.add_output(output);
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
