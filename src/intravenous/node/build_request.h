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
#include <intravenous/node/port_constraints.h>

#include <array>
#include <cstddef>
#include <concepts>
#include <cstdint>
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
    ReflectedPortConstraintSink port_constraint_sink() const;
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
        return count_continuous_ports(Node::inputs());
    } else {
        return 0;
    }
}

template<typename Node>
consteval std::size_t reflected_sample_output_count()
{
    if constexpr (has_outputs<Node> && has_constexpr_port_configs<Node>) {
        return count_port_configs(Node::outputs(), [](auto const& config) {
            return is_continuous(config) && is_tick(config);
        });
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
        return count_port_configs(Node::outputs(), [](auto const& config) {
            return !is_continuous(config) && is_tick(config);
        });
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

template<typename Node, std::size_t Index>
consteval bool reflected_sample_output_is_recording()
{
    std::size_t tick_sample_index = 0;
    std::optional<bool> result;
    for_each_port_config(Node::outputs(), [&](auto const& config) {
        if (is_continuous(config) && is_tick(config) && !result) {
            if (tick_sample_index++ == Index) result = is_persisted(config);
        }
    });
    if (result) {
        return *result;
    }
    throw "reflected Tick sample output index is out of bounds";
}

template<typename Node, std::size_t Index>
consteval bool reflected_event_output_is_recording()
{
    std::size_t tick_event_index = 0;
    std::optional<bool> result;
    for_each_port_config(Node::outputs(), [&](auto const& config) {
        if (!is_continuous(config) && is_tick(config) && !result) {
            if (tick_event_index++ == Index) result = is_persisted(config);
        }
    });
    if (result) {
        return *result;
    }
    throw "reflected Tick event output index is out of bounds";
}

template<typename Node, std::size_t... Index>
IV_FORCEINLINE auto make_tick_recording_bindings(
    ReflectedNodeTickContext const& ctx,
    std::index_sequence<Index...>)
{
    TickRecordingBindings<Node> result{};
    (([&] {
        if constexpr (static_output_port_is_recording_at<Node, Index>()) {
            if constexpr (static_output_port_kind_at<Node, Index>()
                == PortKind::sample) {
                constexpr auto output_index =
                    static_realtime_sample_output_port_index_at<Node, Index>();
                constexpr auto recording_index =
                    static_recording_sample_output_index_at<Node, Index>();
                result.sample_outputs[recording_index] =
                    &ctx.sample_output_bindings.pointer[output_index].disposition;
            } else {
                constexpr auto output_index =
                    static_realtime_event_output_port_index_at<Node, Index>();
                constexpr auto recording_index =
                    static_recording_event_output_index_at<Node, Index>();
                result.event_outputs[recording_index] =
                    &ctx.event_output_bindings.pointer[output_index].disposition;
            }
        }
    }()), ...);
    return result;
}

template<typename Node>
IV_FORCEINLINE auto make_tick_recording_bindings(
    ReflectedNodeTickContext const& ctx)
{
    if constexpr (has_outputs<Node>) {
        return make_tick_recording_bindings<Node>(
            ctx,
            std::make_index_sequence<
                port_config_count_v<decltype(Node::outputs())>>{});
    } else {
        return TickRecordingBindings<Node>{};
    }
}

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
    GlobalIndex index)
{
    return InputPort{
        reflected_sample_storage_view(binding.storage),
        binding.history,
        binding.read_latency,
        index,
    };
}

template<bool Recording>
IV_FORCEINLINE OutputPort reflected_sample_output_port(
    ReflectedSampleOutputPortBinding const& binding,
    GlobalIndex index)
{
    if constexpr (Recording) {
        binding.disposition = OutputDisposition::untouched;
    }
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
    GlobalIndex index,
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
    GlobalIndex index,
    std::index_sequence<I...>)
{
    return std::array<OutputPort, sizeof...(I)>{
        reflected_sample_output_port<
            reflected_sample_output_is_recording<Node, I>()>(
            ctx.sample_output_bindings.pointer[I],
            index)...
    };
}

template<typename Node, typename Fn>
IV_FORCEINLINE void with_reflected_sample_ports(
    ReflectedNodeTickContext const& ctx,
    GlobalIndex index,
    Fn&& fn)
{
    static_assert(has_constexpr_port_configs<Node>,
        "concrete node ports must be declared by static constexpr inputs() and outputs()");
    constexpr auto input_count = reflected_sample_input_count_v<Node>;
    constexpr auto output_count = reflected_sample_output_count_v<Node>;
    IV_ASSERT(
        ctx.sample_input_bindings.size() == input_count,
        "reflected sample input storage count does not match node declaration");
    IV_ASSERT(
        ctx.sample_output_bindings.size() == output_count,
        "reflected sample output storage count does not match node declaration");

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

template<typename Node, std::size_t N, std::size_t... I>
IV_FORCEINLINE void initialize_reflected_event_outputs(
    ReflectedEventOutputPorts<N>& result,
    ReflectedNodeTickContext const& ctx,
    GlobalIndex index,
    std::size_t block_size,
    std::index_sequence<I...>)
{
    static_assert(N == sizeof...(I));
    (([&] {
        auto const& binding = ctx.event_output_bindings.pointer[I];
        constexpr bool recording =
            reflected_event_output_is_recording<Node, I>();
        if constexpr (recording) {
            binding.disposition = OutputDisposition::untouched;
        }
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
    GlobalIndex index,
    std::size_t block_size,
    Fn&& fn)
{
    static_assert(has_constexpr_port_configs<Node>,
        "concrete node ports must be declared by static constexpr inputs() and outputs()");
    constexpr auto input_count = reflected_event_input_count_v<Node>;
    constexpr auto output_count = reflected_event_output_count_v<Node>;
    IV_ASSERT(
        ctx.event_input_bindings.size() == input_count,
        "reflected event input storage count does not match node declaration");
    IV_ASSERT(
        ctx.event_output_bindings.size() == output_count,
        "reflected event output storage count does not match node declaration");

    ReflectedEventInputPorts<input_count> inputs;
    ReflectedEventOutputPorts<output_count> outputs;
    initialize_reflected_event_inputs(
        inputs, ctx, std::make_index_sequence<input_count>{});
    initialize_reflected_event_outputs<Node>(
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
    GlobalIndex index,
    std::size_t block_size)
{
    auto const& node = *static_cast<Node const*>(node_data);
    with_reflected_sample_ports<Node>(
        ctx,
        index,
        [&](std::span<InputPort> inputs, std::span<OutputPort> outputs) {
            with_reflected_event_ports<Node>(
                ctx,
                index,
                block_size,
                [&](std::span<EventInputPort> event_inputs,
                    std::span<EventOutputPort> event_outputs) {
                    do_tick_block(node, TickBlockContext<Node> {
                        TickContext<Node> {
                            TickPortBindings<Node>{
                                .sample_inputs = inputs,
                                .sample_outputs = outputs,
                                .event_inputs = event_inputs,
                                .event_outputs = event_outputs,
                                .random_access_inputs = ctx.random_access_inputs,
                                .random_access_event_inputs =
                                    ctx.random_access_event_inputs,
                                .recording =
                                    make_tick_recording_bindings<Node>(ctx),
                            },
                            ctx.sample_rate,
                            ctx.scc_feedback_latency,
                            ctx.state,
                        },
                        index,
                        block_size,
                    });
                });
        });
}

template<class Node>
IV_FORCEINLINE void skip_node_block(
    void const* node_data,
    ReflectedNodeTickContext const& ctx,
    GlobalIndex index,
    std::size_t block_size)
{
    auto const& node = *static_cast<Node const*>(node_data);
    with_reflected_sample_ports<Node>(
        ctx,
        index,
        [&](std::span<InputPort> inputs, std::span<OutputPort> outputs) {
            with_reflected_event_ports<Node>(
                ctx,
                index,
                block_size,
                [&](std::span<EventInputPort> event_inputs,
                    std::span<EventOutputPort> event_outputs) {
                    do_skip_block(node, SkipBlockContext<Node> {
                        TickContext<Node> {
                            TickPortBindings<Node>{
                                .sample_inputs = inputs,
                                .sample_outputs = outputs,
                                .event_inputs = event_inputs,
                                .event_outputs = event_outputs,
                                .random_access_inputs = ctx.random_access_inputs,
                                .random_access_event_inputs =
                                    ctx.random_access_event_inputs,
                                .recording =
                                    make_tick_recording_bindings<Node>(ctx),
                            },
                            ctx.sample_rate,
                            ctx.scc_feedback_latency,
                            ctx.state,
                        },
                        index,
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
consteval bool declares_sequential_continuous_inputs()
{
    if constexpr (has_inputs<Node>) {
        return any_port_config(Node::inputs(), [](auto const& input) {
            using Config = std::remove_cvref_t<decltype(input)>;
            if constexpr (AuthoredContinuousInputConfig<Config>) {
                return Config::access == InputAccessKind::sequential;
            } else {
                return false;
            }
        });
    } else {
        return false;
    }
}

template<class Node, std::size_t DeclarationIndex>
inline constexpr auto sequential_input_options = [] consteval {
    auto const inputs = Node::inputs();
    return port_config_at<DeclarationIndex>(inputs).options;
}();

template<class Extension>
bool initialize_sequential_storage(
    typename Extension::SequentialInputOptions const& options,
    std::byte* destination,
    std::size_t destination_bytes,
    std::size_t elements_per_value)
{
    using Element = typename Extension::storage_element;
    if ((destination == nullptr && destination_bytes != 0)
        || reinterpret_cast<std::uintptr_t>(destination) % alignof(Element) != 0
        || destination_bytes % sizeof(Element) != 0) {
        return false;
    }
    return Extension::initialize_sequential_neutral(
        options,
        std::span<Element>{
            reinterpret_cast<Element*>(destination),
            destination_bytes / sizeof(Element)},
        elements_per_value);
}

template<class Extension>
bool combine_sequential_storage(
    typename Extension::SequentialInputOptions const& options,
    std::byte* destination,
    std::byte const* source,
    std::size_t byte_count,
    std::size_t elements_per_value)
{
    using Element = typename Extension::storage_element;
    if (((destination == nullptr || source == nullptr) && byte_count != 0)
        || reinterpret_cast<std::uintptr_t>(destination) % alignof(Element) != 0
        || reinterpret_cast<std::uintptr_t>(source) % alignof(Element) != 0
        || byte_count % sizeof(Element) != 0) {
        return false;
    }
    auto const element_count = byte_count / sizeof(Element);
    return Extension::combine_sequential_values(
        options,
        std::span<Element>{
            reinterpret_cast<Element*>(destination), element_count},
        std::span<Element const>{
            reinterpret_cast<Element const*>(source), element_count},
        elements_per_value);
}

template<class Node, std::size_t DeclarationIndex, std::size_t TypeIndex>
bool initialize_sequential_input_type(
    ContinuousValueTypeId value_type,
    std::byte* destination,
    std::size_t destination_bytes,
    std::size_t elements_per_value)
{
    using Config = std::remove_cvref_t<decltype(port_config_at<DeclarationIndex>(
        std::declval<decltype(Node::inputs()) const&>()))>;
    using Type = std::tuple_element_t<TypeIndex, typename Config::value_types>;
    if (value_type != continuous_value_type_id_v<Type>) return false;
    return initialize_sequential_storage<ContinuousValueType<Type>>(
        std::get<TypeIndex>(
            sequential_input_options<Node, DeclarationIndex>),
        destination,
        destination_bytes,
        elements_per_value);
}

template<class Node, std::size_t DeclarationIndex, std::size_t... TypeIndex>
bool initialize_sequential_input_types(
    ContinuousValueTypeId value_type,
    std::byte* destination,
    std::size_t destination_bytes,
    std::size_t elements_per_value,
    std::index_sequence<TypeIndex...>)
{
    return (initialize_sequential_input_type<
        Node, DeclarationIndex, TypeIndex>(
            value_type,
            destination,
            destination_bytes,
            elements_per_value) || ...);
}

template<class Node, std::size_t DeclarationIndex, std::size_t TypeIndex>
bool combine_sequential_input_type(
    ContinuousValueTypeId value_type,
    std::byte* destination,
    std::byte const* source,
    std::size_t byte_count,
    std::size_t elements_per_value)
{
    using Config = std::remove_cvref_t<decltype(port_config_at<DeclarationIndex>(
        std::declval<decltype(Node::inputs()) const&>()))>;
    using Type = std::tuple_element_t<TypeIndex, typename Config::value_types>;
    if (value_type != continuous_value_type_id_v<Type>) return false;
    return combine_sequential_storage<ContinuousValueType<Type>>(
        std::get<TypeIndex>(
            sequential_input_options<Node, DeclarationIndex>),
        destination,
        source,
        byte_count,
        elements_per_value);
}

template<class Node, std::size_t DeclarationIndex, std::size_t... TypeIndex>
bool combine_sequential_input_types(
    ContinuousValueTypeId value_type,
    std::byte* destination,
    std::byte const* source,
    std::size_t byte_count,
    std::size_t elements_per_value,
    std::index_sequence<TypeIndex...>)
{
    return (combine_sequential_input_type<
        Node, DeclarationIndex, TypeIndex>(
            value_type,
            destination,
            source,
            byte_count,
            elements_per_value) || ...);
}

template<class Node, std::size_t DeclarationIndex>
bool initialize_sequential_input_declaration(
    std::size_t continuous_input,
    ContinuousValueTypeId value_type,
    std::byte* destination,
    std::size_t destination_bytes,
    std::size_t elements_per_value)
{
    using Config = std::remove_cvref_t<decltype(port_config_at<DeclarationIndex>(
        std::declval<decltype(Node::inputs()) const&>()))>;
    if constexpr (AuthoredContinuousInputConfig<Config>
        && Config::access == InputAccessKind::sequential) {
        if (continuous_input
            != static_sample_input_port_index_at<Node, DeclarationIndex>()) {
            return false;
        }
        return initialize_sequential_input_types<Node, DeclarationIndex>(
            value_type,
            destination,
            destination_bytes,
            elements_per_value,
            std::make_index_sequence<
                std::tuple_size_v<typename Config::value_types>>{});
    } else {
        return false;
    }
}

template<class Node, std::size_t DeclarationIndex>
bool combine_sequential_input_declaration(
    std::size_t continuous_input,
    ContinuousValueTypeId value_type,
    std::byte* destination,
    std::byte const* source,
    std::size_t byte_count,
    std::size_t elements_per_value)
{
    using Config = std::remove_cvref_t<decltype(port_config_at<DeclarationIndex>(
        std::declval<decltype(Node::inputs()) const&>()))>;
    if constexpr (AuthoredContinuousInputConfig<Config>
        && Config::access == InputAccessKind::sequential) {
        if (continuous_input
            != static_sample_input_port_index_at<Node, DeclarationIndex>()) {
            return false;
        }
        return combine_sequential_input_types<Node, DeclarationIndex>(
            value_type,
            destination,
            source,
            byte_count,
            elements_per_value,
            std::make_index_sequence<
                std::tuple_size_v<typename Config::value_types>>{});
    } else {
        return false;
    }
}

template<class Node, std::size_t... DeclarationIndex>
bool initialize_node_sequential_input_neutral_impl(
    std::size_t continuous_input,
    ContinuousValueTypeId value_type,
    std::byte* destination,
    std::size_t destination_bytes,
    std::size_t elements_per_value,
    std::index_sequence<DeclarationIndex...>)
{
    return (initialize_sequential_input_declaration<Node, DeclarationIndex>(
        continuous_input,
        value_type,
        destination,
        destination_bytes,
        elements_per_value) || ...);
}

template<class Node>
bool initialize_node_sequential_input_neutral(
    std::size_t continuous_input,
    ContinuousValueTypeId value_type,
    std::byte* destination,
    std::size_t destination_bytes,
    std::size_t elements_per_value)
{
    return initialize_node_sequential_input_neutral_impl<Node>(
        continuous_input,
        value_type,
        destination,
        destination_bytes,
        elements_per_value,
        std::make_index_sequence<
            port_config_count_v<decltype(Node::inputs())>>{});
}

template<class Node, std::size_t... DeclarationIndex>
bool combine_node_sequential_input_values_impl(
    std::size_t continuous_input,
    ContinuousValueTypeId value_type,
    std::byte* destination,
    std::byte const* source,
    std::size_t byte_count,
    std::size_t elements_per_value,
    std::index_sequence<DeclarationIndex...>)
{
    return (combine_sequential_input_declaration<Node, DeclarationIndex>(
        continuous_input,
        value_type,
        destination,
        source,
        byte_count,
        elements_per_value) || ...);
}

template<class Node>
bool combine_node_sequential_input_values(
    std::size_t continuous_input,
    ContinuousValueTypeId value_type,
    std::byte* destination,
    std::byte const* source,
    std::size_t byte_count,
    std::size_t elements_per_value)
{
    return combine_node_sequential_input_values_impl<Node>(
        continuous_input,
        value_type,
        destination,
        source,
        byte_count,
        elements_per_value,
        std::make_index_sequence<
            port_config_count_v<decltype(Node::inputs())>>{});
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
        .initialize_sequential_input_neutral = [] {
            if constexpr (declares_sequential_continuous_inputs<Node>()) {
                return &initialize_node_sequential_input_neutral<Node>;
            } else {
                return static_cast<bool (*)(
                    std::size_t,
                    ContinuousValueTypeId,
                    std::byte*,
                    std::size_t,
                    std::size_t)>(nullptr);
            }
        }(),
        .combine_sequential_input_values = [] {
            if constexpr (declares_sequential_continuous_inputs<Node>()) {
                return &combine_node_sequential_input_values<Node>;
            } else {
                return static_cast<bool (*)(
                    std::size_t,
                    ContinuousValueTypeId,
                    std::byte*,
                    std::byte const*,
                    std::size_t,
                    std::size_t)>(nullptr);
            }
        }(),
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
        for_each_port_config(Node::inputs(), [&](auto const& input) {
            sink.add_input(erase_port_config(input));
        });
    }
    if constexpr (has_outputs<Node>) {
        for_each_port_config(Node::outputs(), [&](auto const& output) {
            sink.add_output(erase_port_config(output));
        });
    }
    ConstrainPortsContext<Node> port_constraints(sink.port_constraint_sink());
    if constexpr (requires {
        { node.constrain_ports(port_constraints) } -> std::same_as<void>;
    }) {
        node.constrain_ports(port_constraints);
    }
    static_assert(replay_declaration_is_valid_v<Node>,
        "intrinsically replayable nodes must author tick() (not tick_block()), "
        "have no State/TockState or RandomAccess inputs, and declare only "
        "Sequential inputs and Tick outputs; resolved timing is validated by "
        "the whole-graph planner");
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
        "Sequential inputs and Tick outputs; resolved timing is validated by "
        "the whole-graph planner");
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
