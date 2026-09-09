#pragma once

#include <intravenous/basic_nodes/routing.h>
#include <intravenous/basic_nodes/type_erased.h>
#include <intravenous/node/lifecycle.h>
#include <intravenous/node/code_key.h>
#include <intravenous/node/config_relocations.h>
#include <intravenous/node/config_storage.h>
#include <intravenous/ports.h>


#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace iv {
struct NodeLayoutBuilder;
struct NodeStateStructure;

struct NodePorts {
    std::vector<InputConfig> sample_inputs {};
    std::vector<OutputConfig> sample_outputs {};
    std::vector<EventInputConfig> event_input_configs {};
    std::vector<EventOutputConfig> event_output_configs {};

    constexpr std::vector<InputConfig> const& inputs() const
    {
        return sample_inputs;
    }

    constexpr std::vector<OutputConfig> const& outputs() const
    {
        return sample_outputs;
    }

    constexpr std::vector<EventInputConfig> const& event_inputs() const
    {
        return event_input_configs;
    }

    constexpr std::vector<EventOutputConfig> const& event_outputs() const
    {
        return event_output_configs;
    }
};

struct ReflectedNodeTickContext {
    std::span<InputPort> inputs {};
    std::span<OutputPort> outputs {};
    std::span<EventInputPort> event_inputs {};
    std::span<EventOutputPort> event_outputs {};
    size_t sample_rate = 48000;
    size_t scc_feedback_latency = 0;
    std::span<std::byte> state {};
};

// Every function is specialized on the node type only. The authored Node
// value is immutable configuration data, so several differently configured
// instances of the same type share one set of callbacks.
struct ReflectedNodeRuntimeOperations {
    void const* node_data = nullptr;
    NodeStateStructure const* state_structure = nullptr;
    size_t (*declare_node)(
        void const*, NodeStateStructure const*, NodeLayoutBuilder&) = nullptr;
    void (*tick_block)(
        void const*,
        ReflectedNodeTickContext const&,
        size_t,
        size_t) = nullptr;
    void (*skip_block)(
        void const*,
        ReflectedNodeTickContext const&,
        size_t,
        size_t) = nullptr;

    constexpr bool valid() const
    {
        return declare_node != nullptr && tick_block != nullptr
            && skip_block != nullptr;
    }
};

struct ReflectedNodeOperations {
    ReflectedNodeRuntimeOperations runtime {};

    constexpr bool valid() const
    {
        return runtime.valid();
    }
};

struct ReflectedNodeDescription {
    NodePorts ports {};
    ReflectedNodeOperations operations {};
    // Node configuration is ordinary immutable C++ data. Keep the object
    // alive independently from the module build stack/JIT generation; the
    // runtime callbacks continue to receive operations.runtime.node_data.
    std::shared_ptr<void const> node_storage {};
    std::shared_ptr<NodeStateStructure const> state_structure_storage {};
    NodeConfigStringRelocations config_string_relocations {};
    NodeCodeKey code_key {};
    std::size_t node_size = 0;
    std::size_t node_alignment = 1;
    std::string_view type_name {};
    size_t internal_latency_samples = 0;
    size_t maximum_block_size = MAX_BLOCK_SIZE;
    std::optional<size_t> default_ttl_samples {};
    bool block_skippable = false;
    // Constants are sources, not executable DSP work.  Carry their authored
    // value through reflection so the compiler can bind their consumers to
    // initialized storage instead of emitting a ticking Constant wrapper.
    std::optional<Sample> static_sample_value {};

    constexpr std::vector<InputConfig> const& inputs() const
    {
        return ports.sample_inputs;
    }

    constexpr std::vector<OutputConfig> const& outputs() const
    {
        return ports.sample_outputs;
    }

    constexpr std::vector<EventInputConfig> const& event_inputs() const
    {
        return ports.event_input_configs;
    }

    constexpr std::vector<EventOutputConfig> const& event_outputs() const
    {
        return ports.event_output_configs;
    }

    constexpr size_t internal_latency() const
    {
        return internal_latency_samples;
    }

    constexpr size_t max_block_size() const
    {
        return maximum_block_size;
    }

    constexpr std::optional<size_t> ttl_samples() const
    {
        return default_ttl_samples;
    }

    constexpr bool can_skip_block() const
    {
        return block_skippable;
    }
};

namespace details {
    template<class Node>
    ReflectedNodeDescription reflect_node(Node const& node);

    template<class Node>
    constexpr ReflectedNodeOperations reflected_node_operations(Node const* node_data);

    template<class Node>
    size_t declare_reflected_node(
        void const* node_data,
        NodeStateStructure const* state_structure,
        NodeLayoutBuilder& builder)
    {
        auto const& node = *static_cast<Node const*>(node_data);
        DeclarationContext<Node> ctx(builder, node);
        if (state_structure) {
            builder.override_node_state_structure(ctx.node_index(), *state_structure);
        }
        if constexpr (details::has_declare<Node>) {
            node.declare(ctx);
        }
        return ctx.node_index();
    }

    template<class Node>
    IV_FORCEINLINE void tick_reflected_node_block(
        void const* node_data,
        ReflectedNodeTickContext const& ctx,
        size_t index,
        size_t block_size)
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
    IV_FORCEINLINE void skip_reflected_node_block(
        void const* node_data,
        ReflectedNodeTickContext const& ctx,
        size_t index,
        size_t block_size)
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
    constexpr ReflectedNodeOperations reflected_node_operations(Node const* node_data)
    {
        return {
            .runtime = {
                .node_data = node_data,
                .state_structure = nullptr,
                .declare_node = &declare_reflected_node<Node>,
                .tick_block = &tick_reflected_node_block<Node>,
                .skip_block = &skip_reflected_node_block<Node>,
            },
        };
    }

    // The callback set and Clang type spelling are properties of Node, not of
    // one authored Node value. NodeCodeKey is intentionally build-local: it
    // joins the graph produced by module_main with the LLVM compiler record
    // emitted by the same compilation. Hot-reload identity remains the type
    // spelling stored separately in graph metadata.
    struct ReflectedNodeTypeMetadata {
        ReflectedNodeOperations operations {};
        NodeCodeKey code_key {};
        std::string_view type_name {};
    };

    template<class Node>
    inline constexpr ReflectedNodeTypeMetadata reflected_node_type_metadata {
        .operations = reflected_node_operations<Node>(nullptr),
        .code_key = node_code_key_v<Node>,
        .type_name = clang_type_name<Node>(),
    };

    struct NodeCompilerRecord {
        NodeCodeKey code_key {};
        ReflectedNodeRuntimeOperations runtime {};
        char const* type_name = nullptr;
        std::size_t type_name_size = 0;
        // This is ABI data for the finalizer only. It tells the finalizer
        // whether structural State metadata is mandatory for this record and
        // gives it an independent ABI check before it publishes that metadata
        // to the runtime graph compiler.
        std::size_t state_size = 0;
        std::size_t state_alignment = 1;
    };

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
            .runtime = reflected_node_operations<Node>(nullptr).runtime,
            .type_name = clang_type_name<Node>().data(),
            .type_name_size = clang_type_name<Node>().size(),
            .state_size = node_state_size<Node>(),
            .state_alignment = node_state_alignment<Node>(),
        };

    // Description is independent of where the node object lives. Authoring owns
    // the immutable config separately from the stack so it can be serialized
    // after module_main returns.
    template<class Node>
    constexpr ReflectedNodeDescription describe_reflected_node(
        Node const& node,
        Node const* node_data)
    {
        ReflectedNodeDescription description;
        auto const& type_metadata = reflected_node_type_metadata<Node>;
        for (auto const& input : get_inputs(node)) {
            description.ports.sample_inputs.push_back(input);
        }
        for (auto const& output : get_outputs(node)) {
            description.ports.sample_outputs.push_back(output);
        }
        for (auto const& input : get_event_inputs(node)) {
            description.ports.event_input_configs.push_back(input);
        }
        for (auto const& output : get_event_outputs(node)) {
            description.ports.event_output_configs.push_back(output);
        }

        description.operations = type_metadata.operations;
        description.operations.runtime.node_data = node_data;
        description.code_key = type_metadata.code_key;
        description.node_size = sizeof(Node);
        description.node_alignment = alignof(Node);
        description.type_name = type_metadata.type_name;
        description.internal_latency_samples = get_internal_latency(node);
        description.maximum_block_size = get_max_block_size(node);
        description.default_ttl_samples = get_ttl_samples(node);
        description.block_skippable = get_can_skip_block(node);
        if constexpr (std::same_as<std::remove_cvref_t<Node>, Constant>) {
            description.static_sample_value = node._value;
        }
        return description;
    }

    template<class Node>
    ReflectedNodeDescription reflect_node(Node const& node)
    {
        using Value = std::remove_cvref_t<Node>;
        // Force the compiler record specialization into the LLVM module. The
        // built graph stores only its NodeCodeKey; iv-module-finalize later
        // resolves that key back to this record's direct function references.
        (void)&node_compiler_record<Value>;

        auto storage = copy_node_config_bytes(
            std::addressof(node), sizeof(Value), alignof(Value));
        auto const* stored = static_cast<Value const*>(storage.get());
        auto description = describe_reflected_node(*stored, stored);
        description.node_storage = std::move(storage);
        return description;
    }
#undef IV_NODE_COMPILER_RECORD_ATTR
} // namespace details
} // namespace iv
