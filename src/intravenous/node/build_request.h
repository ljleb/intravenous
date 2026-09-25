#pragma once

// The module-side node boundary.  Templates here provide the genuinely
// type-specific work: construct a node value, emit its compiler record, and
// enumerate its declared properties.  The builder library owns the resulting
// graph description, dynamic containers, configuration storage, and
// validation.

#include <intravenous/basic_nodes/constant.h>
#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/node/coverage_port_context.h>
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
    void set_intrinsically_replayable(bool) const;
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
        std::size_t count = 0;
        for (auto const& config : Node::outputs()) {
            if (is_sample(config) && is_tick(config.production)) ++count;
        }
        return count;
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
        std::size_t count = 0;
        for (auto const& config : Node::outputs()) {
            if (!is_sample(config) && is_tick(config.production)) ++count;
        }
        return count;
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
    ReflectedSamplePortStorageBinding const& binding)
{
    std::array<SampleChannelStorageView, maximum_supported_channel_count>
        channels{};
    auto const count = channel_count(binding.channel_layout);
    for (std::size_t channel = 0; channel < count; ++channel) {
        channels[channel] = SampleChannelStorageView{
            .storage = reinterpret_cast<Sample*>(
                binding.channels[channel].storage),
            .frame_capacity = binding.channels[channel].frame_capacity,
            .frame_stride = binding.channels[channel].frame_stride,
            .frame_delay = binding.channels[channel].frame_delay,
        };
    }
    return SamplePortStorageView{
        channels,
        binding.storage_latency,
        binding.channel_layout,
        binding.frame_capacity,
    };
}

IV_FORCEINLINE InputPort reflected_sample_input_port(
    ReflectedSampleInputPortBinding const& binding,
    SampleIndex index)
{
    return InputPort{
        reflected_sample_storage_view(binding.storage),
        binding.history,
        binding.read_latency,
        index,
    };
}

IV_FORCEINLINE OutputPort reflected_sample_output_port(
    ReflectedSampleOutputPortBinding const& binding,
    SampleIndex index)
{
    return OutputPort{
        reflected_sample_storage_view(binding.storage),
        binding.history,
        index,
        binding.latency,
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
    static_assert(has_constexpr_port_configs<Node>,
        "concrete node ports must be declared by static constexpr inputs() and outputs()");
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

template<std::size_t N>
struct ReflectedEventInputPorts {
    std::array<EventSharedPortData, N> shared{};
    std::array<EventInputPort, N> ports{};
};

template<std::size_t N>
struct ReflectedEventOutputPorts {
    std::array<EventSharedPortData, N> shared{};
    std::array<EventOutputPort, N> ports{};
    std::array<std::size_t*, N> write_indices{};
};

IV_FORCEINLINE std::span<TimedEvent> reflected_event_buffer(
    ReflectedEventPortStorageBinding const& binding)
{
    auto* events = reinterpret_cast<TimedEvent*>(
        binding.storage + binding.events_offset);
    return {events, binding.event_capacity};
}

IV_FORCEINLINE std::size_t* reflected_event_count(
    ReflectedEventPortStorageBinding const& binding)
{
    return reinterpret_cast<std::size_t*>(
        binding.storage + binding.count_offset);
}

IV_FORCEINLINE std::size_t* reflected_event_read_index(
    ReflectedEventPortStorageBinding const& binding)
{
    return reinterpret_cast<std::size_t*>(
        binding.storage + binding.read_index_offset);
}

IV_FORCEINLINE std::size_t* reflected_event_write_index(
    ReflectedEventPortStorageBinding const& binding)
{
    return reinterpret_cast<std::size_t*>(
        binding.storage + binding.write_index_offset);
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
        auto const read_index = binding.persistent_ring
            ? *reflected_event_read_index(binding)
            : std::size_t{0};
        auto const write_index = binding.persistent_ring
            ? *reflected_event_write_index(binding)
            : *reflected_event_count(binding);
        result.shared[I] = EventSharedPortData{
            reflected_event_buffer(binding),
            read_index,
            write_index,
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
        auto* write_index = binding.storage.persistent_ring
            ? reflected_event_write_index(binding.storage)
            : reflected_event_count(binding.storage);
        auto const initial_read = binding.storage.persistent_ring
            ? *reflected_event_read_index(binding.storage)
            : std::size_t{0};
        auto const initial_write = binding.append_existing ? *write_index : 0;
        if (!binding.append_existing) {
            *write_index = 0;
        }
        result.write_indices[I] = write_index;
        auto output_buffer = reflected_event_buffer(
            binding.storage);
        result.shared[I] = EventSharedPortData{
            output_buffer,
            initial_read,
            initial_write,
            binding.storage.type,
        };
        result.ports[I] = EventOutputPort{
            result.shared[I],
            binding.source_type,
            binding.history,
            binding.latency,
            binding.overflow_count,
        };
        result.ports[I].begin_block(index, block_size);
    }()), ...);
}

template<std::size_t N>
IV_FORCEINLINE void commit_reflected_event_outputs(
    ReflectedEventOutputPorts<N>& outputs)
{
    for (std::size_t i = 0; i < N; ++i) {
        *outputs.write_indices[i] = outputs.shared[i].write_index;
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
    static_assert(has_constexpr_port_configs<Node>,
        "concrete node ports must be declared by static constexpr inputs() and outputs()");
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
                            .random_access_inputs = ctx.random_access_inputs,
                            .random_access_event_inputs = ctx.random_access_event_inputs,
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
                            .random_access_inputs = ctx.random_access_inputs,
                            .random_access_event_inputs = ctx.random_access_event_inputs,
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
IV_FORCEINLINE void tock_node_coverage(
    void const* node_data, ReflectedNodeTockCoverageContext const& reflected)
{
    auto const& node = *static_cast<Node const*>(node_data);
    TockCoverageContext<Node> context{
        .inputs = reflected.inputs,
        .outputs = reflected.outputs,
        .event_inputs = reflected.event_inputs,
        .event_outputs = reflected.event_outputs,
        .background_state_storage = reflected.background_state_storage,
        .sample_rate = reflected.sample_rate,
    };
    do_tock_coverage(node, context);
}

template<class Node>
IV_FORCEINLINE void propagate_node_forward_coverage(
    void const* node_data, ReflectedNodeForwardCoverageContext const& reflected)
{
    auto const& node = *static_cast<Node const*>(node_data);
    PropagateForwardCoverageContext<Node> context{
        .inputs = reflected.inputs,
        .outputs = reflected.outputs,
        .event_inputs = reflected.event_inputs,
        .event_outputs = reflected.event_outputs,
        .local_state_changed = reflected.local_state_changed,
        .sample_rate = reflected.sample_rate,
    };
    do_propagate_forward_coverage(node, context);
}

template<class Node>
IV_FORCEINLINE void propagate_node_reverse_coverage(
    void const* node_data, ReflectedNodeReverseCoverageContext const& reflected)
{
    auto const& node = *static_cast<Node const*>(node_data);
    PropagateReverseCoverageContext<Node> context{
        .inputs = reflected.inputs,
        .outputs = reflected.outputs,
        .event_inputs = reflected.event_inputs,
        .event_outputs = reflected.event_outputs,
        .sample_rate = reflected.sample_rate,
    };
    do_propagate_reverse_coverage(node, context);
}

template<class Node>
consteval auto node_tock_coverage_operation()
{
    if constexpr (details::declares_tock_outputs_v<Node>) {
        static_assert(details::has_tock_coverage<Node>,
            "background-output node has no valid tock_coverage implementation");
        return &tock_node_coverage<Node>;
    } else {
        return static_cast<void (*)(
            void const*, ReflectedNodeTockCoverageContext const&)>(nullptr);
    }
}

template<class Node>
consteval auto node_propagate_forward_coverage_operation()
{
    if constexpr (details::declares_tock_outputs_v<Node>) {
        static_assert(details::has_propagate_forward_coverage<Node>,
            "background-output node has no exact propagate_forward_coverage implementation");
        return &propagate_node_forward_coverage<Node>;
    } else {
        return static_cast<void (*)(
            void const*, ReflectedNodeForwardCoverageContext const&)>(nullptr);
    }
}

template<class Node>
consteval auto node_propagate_reverse_coverage_operation()
{
    if constexpr (details::declares_tock_outputs_v<Node>
        && details::declares_random_access_inputs_v<Node>) {
        return &propagate_node_reverse_coverage<Node>;
    } else {
        return static_cast<void (*)(
            void const*, ReflectedNodeReverseCoverageContext const&)>(nullptr);
    }
}

template<class Node>
constexpr NodeCompilerOperations node_compiler_operations()
{
    return {
        .declare_node = &declare_node<Node>,
        .tick_block = &tick_node_block<Node>,
        .skip_block = &skip_node_block<Node>,
        .tock_coverage = node_tock_coverage_operation<Node>(),
        .propagate_forward_coverage =
            node_propagate_forward_coverage_operation<Node>(),
        .propagate_reverse_coverage =
            node_propagate_reverse_coverage_operation<Node>(),
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
consteval std::size_t node_background_state_size()
{
    using TockState = typename NodeBackgroundState<Node>::Type;
    if constexpr (std::is_void_v<TockState>) {
        return 0;
    } else {
        return sizeof(TockState);
    }
}

template<class Node>
consteval std::size_t node_background_state_alignment()
{
    using TockState = typename NodeBackgroundState<Node>::Type;
    if constexpr (std::is_void_v<TockState>) {
        return 1;
    } else {
        return alignof(TockState);
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
        .background_state_size = node_background_state_size<Node>(),
        .background_state_alignment = node_background_state_alignment<Node>(),
        .intrinsically_replayable = intrinsically_replayable_v<Node>,
    };

#undef IV_NODE_COMPILER_RECORD_ATTR

template<class Node>
void describe_node(void const* node_data, NodeDescriptionSink& sink)
{
    auto const& node = *static_cast<Node const*>(node_data);
    static_assert(has_constexpr_port_configs<Node>,
        "concrete node ports must be declared by static constexpr inputs() and outputs()");
    if constexpr (has_inputs<Node>) {
        for (InputConfig const& input : Node::inputs()) {
            sink.add_input(input);
        }
    }
    if constexpr (has_outputs<Node>) {
        for (OutputConfig const& output : Node::outputs()) {
            sink.add_output(output);
        }
    }
    static_assert(replay_declaration_is_valid_v<Node>,
        "intrinsically replayable nodes must author tick() (not tick_block()), "
        "have no State/TockState or RandomAccess inputs, and declare only "
        "pointwise Sequential inputs and Tick outputs with zero history/latency");
    sink.set_internal_latency(get_internal_latency(node));
    sink.set_intrinsically_replayable(intrinsically_replayable_v<Node>);
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
    static_assert(has_constexpr_port_configs<Value>,
        "concrete node ports must be declared by static constexpr inputs() and outputs()");
    static_assert(replay_declaration_is_valid_v<Value>,
        "intrinsically replayable nodes must author tick() (not tick_block()), "
        "have no State/TockState or RandomAccess inputs, and declare only "
        "pointwise Sequential inputs and Tick outputs with zero history/latency");
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
