#pragma once
#include "basic_nodes/type_erased.h"
#include "node.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <variant>
#include <numeric>
#include <cstdint>
#include <string>
#include <string_view>


namespace iv {
    struct PortId {
        size_t node;
        size_t port;

        PortId(size_t node = 0, size_t port = 0) :
            node(node), port(port)
        {}

        bool operator==(PortId const&) const = default;
    };

    struct GraphEdge {
        PortId source, target;
        
        GraphEdge(PortId source = {}, PortId target = {}) :
            source(source), target(target)
        {}

        bool operator==(GraphEdge const&) const = default;
    };
}

template<>
struct std::hash<iv::PortId>
{
    std::hash<size_t> size_t_hash;
    std::size_t operator()(const iv::PortId& p) const
    {
        return size_t_hash(p.node) ^ (~size_t_hash(p.port) - 1);
    }
};

template<>
struct std::hash<iv::GraphEdge> {
    std::hash<iv::PortId> port_id_hash;
    std::size_t operator()(const iv::GraphEdge& e) const
    {
        return port_id_hash(e.source) ^ (~port_id_hash(e.target) - 1);
    }
};

namespace iv {
    static constexpr size_t GRAPH_ID = std::numeric_limits<size_t>::max();

    class LatencyAccumulator {
        std::unordered_map<PortId, size_t> _input_port_global_latencies;

    public:
        void align_latencies(auto& node, size_t node_i, auto const& input_configs, auto const& output_configs, auto const& target_of)
        {
            size_t node_global_latency = 0;
            size_t const num_inputs = input_configs.size();
            for (size_t in_port = 0; in_port < num_inputs; ++in_port)
            {
                node_global_latency = std::max(node_global_latency, _input_port_global_latencies[{ node_i, in_port }]);
            }

            if (node_i == GRAPH_ID) return;

            node_global_latency += get_internal_latency(node);

            size_t const num_outputs = output_configs.size();
            for (size_t out_port = 0; out_port < num_outputs; ++out_port)
            {
                size_t output_latency = node_global_latency + output_configs[out_port].latency;
                if (auto it = target_of.find({ node_i, out_port }); it != target_of.end())
                {
                    _input_port_global_latencies[it->second] = output_latency;
                }
            }
        }

        size_t delay_input(PortId input, size_t extra_delay)
        {
            return _input_port_global_latencies.at(input) += extra_delay;
        }

        size_t get_input_latency(PortId input) const
        {
            return _input_port_global_latencies.at(input);
        }
    };

    struct Graph {
        using Nodes = std::vector<TypeErasedNode>;
        using Edges = std::unordered_set<GraphEdge>;

        struct GraphState : public NodeState {
            std::span<NodeState> node_states;
        };

        Nodes _nodes;
        Edges _edges;
        std::vector<InputConfig> _public_inputs;
        std::vector<OutputConfig> _public_outputs;

        struct ReplayState {
            std::span<size_t> node_buffer_sizes;
#ifndef NDEBUG
            std::span<AllocationTrace> node_buffer_traces;
#endif
        };

        static size_t calculate_port_buffer_size(size_t latency, size_t input_history, size_t output_history)
        {
            size_t min_size = 1 + latency + std::max(input_history, output_history);
            size_t pow2_size = size_t(1) << size_t(std::ceil(std::log2(min_size)));
            return pow2_size;
        };

        auto make_source_target_edge_maps() const
        {
            std::unordered_map<PortId, PortId> source_of;
            std::unordered_map<PortId, PortId> target_of;
            for (GraphEdge const& edge : _edges)
            {
                source_of[edge.target] = edge.source;
                target_of[edge.source] = edge.target;
            }
            return std::make_tuple(std::move(source_of), std::move(target_of));
        }

        static std::string make_init_buffer_id(Graph const* graph, std::string_view suffix)
        {
            return "__graph_init:" + std::to_string(reinterpret_cast<uintptr_t>(graph)) + ":" + std::string(suffix);
        }

        ReplayState register_replay_state(InitBufferContext& context, size_t num_nodes) const
        {
            ReplayState replay_state{
                .node_buffer_sizes = context.register_init_buffer<size_t>(
                    make_init_buffer_id(this, "node_buffer_sizes"),
                    num_nodes
                ),
            };
#ifndef NDEBUG
            replay_state.node_buffer_traces = context.register_init_buffer<AllocationTrace>(
                make_init_buffer_id(this, "node_buffer_traces"),
                num_nodes
            );
#endif
            return replay_state;
        }

        template<typename Allocator>
        static NodeState& node_state_for(Allocator& allocator, GraphState& graph_state, size_t node_i)
        {
            return (node_i == GRAPH_ID)
                ? static_cast<NodeState&>(graph_state)
                : allocator.at(graph_state.node_states, node_i);
        }

        template<typename NodeLike>
        static auto make_node_configs(
            NodeLike const& node,
            size_t node_i,
            std::span<InputConfig const> private_input_configs
        )
        {
            std::vector<InputConfig> input_configs;
            std::vector<OutputConfig> output_configs;

            if (node_i == GRAPH_ID)
            {
                input_configs.assign(private_input_configs.begin(), private_input_configs.end());
            }
            else
            {
                input_configs.assign_range(get_inputs(node));
                output_configs.assign_range(get_outputs(node));
            }

            return std::make_pair(std::move(input_configs), std::move(output_configs));
        }

        template<typename Fn>
        void for_each_graph_node(Fn&& fn) const
        {
            for (size_t node_i = 0; node_i < _nodes.size(); ++node_i) {
                fn(_nodes[node_i], node_i);
            }
            fn(*this, GRAPH_ID);
        }

    public:
        template<class InputConfigs, class OutputConfigs>
        explicit Graph(Nodes nodes, Edges edges, InputConfigs&& inputs, OutputConfigs&& outputs) :
            _nodes(std::move(nodes)),
            _edges(std::move(edges)),
            _public_inputs(std::cbegin(inputs), std::cend(inputs)),
            _public_outputs(std::cbegin(outputs), std::cend(outputs))
        {
        }

        constexpr auto inputs() const
        {
            return _public_inputs;
        }

        constexpr auto outputs() const
        {
            return _public_outputs;
        }

        constexpr auto num_inputs() const
        {
            return _public_inputs.size();
        }

        constexpr auto num_outputs() const
        {
            return _public_outputs.size();
        }

        template<typename Allocator>
        void init_buffer(Allocator& allocator, InitBufferContext& context) const
        {
            /*
            * struct MemoryLayout {
            *     GraphState;    // private inputs outputs pointers
            *     NodeState[n];
            *     OutputPort[pi];  // private outputs connected to public inputs
            *     struct {
            *         // i, o, s and b specific to each node
            *         SharedPortData[i];
            *         OutputPort[o];
            *         InputPort[i];
            *
            *         // input port samples, not output port samples
            *         // honestly not sure which of the two the samples buffer should be stored with
            *         Sample[s];
            *         std::byte[b];
            *     }[n];
            *     SharedPortData[po];
            *     InputPort[po];  // private inputs connected to public outputs
            *     Sample[ps];     // samples for private inputs
            * };
            */
            size_t num_nodes = _nodes.size();
            ReplayState replay_state = register_replay_state(context, num_nodes);

            std::vector<InputConfig> private_input_configs(num_outputs());
            OutputConfig private_outputs_config;

            GraphState& graph_state = allocator.template new_object<GraphState>();
            allocator.assign(graph_state.node_states, allocator.template new_array<NodeState>(num_nodes));
            allocator.assign(graph_state.outputs, allocator.template allocate_array<OutputPort>(num_inputs()));

            auto [source_of, target_of] = make_source_target_edge_maps();

            LatencyAccumulator latency_accumulator;
            std::unordered_map<PortId, std::span<Sample>> input_ports_samples;
            std::unordered_map<size_t, std::span<SharedPortData>> node_inputs_port_data;

            auto allocate_node_state = [&](auto const& node, size_t node_i)
            {
                NodeState& node_state = node_state_for(allocator, graph_state, node_i);
                auto [input_configs, output_configs] = make_node_configs(
                    node,
                    node_i,
                    private_input_configs
                );
                size_t const num_inputs = input_configs.size();
                size_t const num_outputs = output_configs.size();
                allocator.assign(node_state.inputs, allocator.template allocate_array<InputPort>(num_inputs));
                if (node_i != GRAPH_ID) allocator.assign(node_state.outputs, allocator.template allocate_array<OutputPort>(num_outputs));
                node_inputs_port_data[node_i] = allocator.template allocate_array<SharedPortData>(num_inputs);

                latency_accumulator.align_latencies(node, node_i, input_configs, output_configs, target_of);

                for (size_t input_i = 0; input_i < num_inputs; ++input_i)
                {
                    PortId this_input { node_i, input_i };
                    InputConfig const& input_config = input_configs[input_i];
                    size_t num_port_samples;

                    if (auto it = source_of.find(this_input); it != source_of.end())
                    {
                        // input is connected to output, let's setup both
                        size_t output_node_i = it->second.node;
                        size_t output_port_i = it->second.port;

                        OutputConfig const output_config = (output_node_i == GRAPH_ID)
                            ? private_outputs_config
                            : get_outputs(_nodes[output_node_i])[output_port_i];

                        size_t const corrected_latency = latency_accumulator.delay_input(this_input, output_config.latency);
                        num_port_samples = calculate_port_buffer_size(corrected_latency, input_config.history, output_config.history);
                    }
                    else
                    {
                        // input is disconnected, no corresponding output port
                        num_port_samples = calculate_port_buffer_size(0, input_config.history, 0);
                    }

                    input_ports_samples.insert({ this_input, allocator.template allocate_array<Sample>(num_port_samples) });
                }

                if (node_i != GRAPH_ID)
                {
                    if (context.mode == InitBufferContext::PassMode::counting) {
                        CountingNonAllocator counter(
                            allocator.get_buffer().data()
#ifndef NDEBUG
                            , &replay_state.node_buffer_traces[node_i],
                            allocator.get_buffer().data()
#endif
                        );
                        do_init_buffer(node, counter, context);
                        auto const node_buffer_size = counter.estimate_buffer_size();
                        replay_state.node_buffer_sizes[node_i] = node_buffer_size;
                        allocator.assign(node_state.buffer, allocator.template allocate_array<std::byte>(node_buffer_size));
                    } else {
                        if (node_i >= replay_state.node_buffer_sizes.size()) {
                            throw std::logic_error("graph node buffer replay ran past the counting-pass record");
                        }
                        allocator.assign(
                            node_state.buffer,
                            allocator.template allocate_array<std::byte>(replay_state.node_buffer_sizes[node_i])
                        );
                    }
                }
            };

            for_each_graph_node(allocate_node_state);

            if (allocator.can_allocate())
            {
                // memory was allocated, now let's initialize it
                auto initialize_node_state = [&](auto const& node, size_t node_i)
                {
                    NodeState& node_state = node_state_for(allocator, graph_state, node_i);
                    auto [input_configs, output_configs] = make_node_configs(
                        node,
                        node_i,
                        private_input_configs
                    );
                    size_t const num_inputs = input_configs.size();
                    std::span<SharedPortData> inputs_port_data = node_inputs_port_data[node_i];

                    for (size_t input_i = 0; input_i < num_inputs; ++input_i)
                    {
                        PortId this_input { node_i, input_i };
                        InputConfig const& input_config = input_configs[input_i];
                        InputPort& input_port = allocator.at(node_state.inputs, input_i);
                        SharedPortData& input_port_data = allocator.at(inputs_port_data, input_i);
                        std::span<Sample> port_samples = input_ports_samples.at(this_input);

                        allocator.fill_n(port_samples, input_config.default_value);

                        if (auto it = source_of.find({ node_i, input_i }); it != source_of.end())
                        {
                            // input is connected to output, let's setup both
                            size_t output_node_i = it->second.node;
                            size_t output_port_i = it->second.port;

                            OutputConfig const output_config = (output_node_i == GRAPH_ID)
                                ? private_outputs_config
                                : get_outputs(_nodes[output_node_i])[output_port_i];

                            OutputPort& output_port = (output_node_i == GRAPH_ID)
                                ? allocator.at(graph_state.outputs, output_port_i)
                                : allocator.at(allocator.at(graph_state.node_states, output_node_i).outputs, output_port_i);

                            size_t corrected_latency = latency_accumulator.get_input_latency(this_input);
                            allocator.construct_at(&input_port_data, port_samples, corrected_latency);
                            allocator.construct_at(&output_port, input_port_data, output_config.history);
                        }
                        else
                        {
                            // input is disconnected: init dummy buffer
                            allocator.construct_at(&input_port_data, port_samples, OutputConfig{}.latency);
                        }
                        allocator.construct_at(&input_port, input_port_data, input_config.history);
                    }

                    if (node_i != GRAPH_ID)
                    {
#ifndef NDEBUG
                        replay_state.node_buffer_traces[node_i].reset_replay();
                        FixedBufferAllocator nested_allocator {
                            node_state.buffer,
                            &replay_state.node_buffer_traces[node_i],
                            node_state.buffer.data(),
                        };
#else
                        FixedBufferAllocator nested_allocator { node_state.buffer };
#endif
                        std::span<std::byte> result = do_init_buffer(node, nested_allocator, context);
                        nested_allocator.validate_trace_consumed();
                        assert(result.data() == node_state.buffer.data() && result.size() == node_state.buffer.size());
                    }
                };

                for_each_graph_node(initialize_node_state);
            }
        }

        template<typename Allocator>
        void init_buffer(Allocator& allocator) const
        {
            if (!allocator.can_allocate()) {
                InitBufferContext context(InitBufferContext::PassMode::counting, allocator.get_buffer());
                init_buffer(allocator, context);
                return;
            }

            CountingNonAllocator counter(allocator.get_buffer().data());
            InitBufferContext counting_context(InitBufferContext::PassMode::counting, counter.get_buffer());
            do_init_buffer(*this, counter, counting_context);
            counting_context.validate_after_counting();

            InitBufferContext init_context = counting_context.make_initializing_context(allocator.get_buffer());
            init_buffer(allocator, init_context);
            init_context.validate_after_initialization();
        }

        void tick(TickState const& state)
        {
            auto& private_state = state.get_state<GraphState>();
            for (size_t i = 0; i < num_inputs(); ++i) {
                private_state.outputs[i].push(state.inputs[i].get());
            }
            size_t num_nodes = _nodes.size();
            for (size_t i = 0; i < num_nodes; ++i)
            {
                _nodes[i].tick({
                    private_state.node_states[i],
                    state.midi,
                    state.index
                });
            }
            for (size_t i = 0; i < num_outputs(); ++i) {
                state.outputs[i].push(private_state.inputs[i].get());
            }
        }

        size_t internal_latency() const
        {
            std::unordered_map<PortId, PortId> target_of;
            for (GraphEdge const& edge : _edges)
            {
                target_of[edge.source] = edge.target;
            }

            std::unordered_map<PortId, size_t> input_global_latencies;
            size_t max_latency = 0;

            auto process_node = [&](auto& node, size_t node_i)
            {
                size_t node_global_latency = 0;

                size_t const num_inputs = (node_i != GRAPH_ID) ? get_num_inputs(node) : num_outputs();
                for (size_t input_port = 0; input_port < num_inputs; ++input_port)
                {
                    node_global_latency = std::max(node_global_latency, input_global_latencies[{ node_i, input_port }]);
                }

                if (node_i == GRAPH_ID) return;

                node_global_latency += get_internal_latency(node);
                size_t const num_outputs = get_num_outputs(node);
                for (size_t output_port = 0; output_port < num_outputs; ++output_port)
                {
                    if (auto it = target_of.find({ node_i, output_port }); it != target_of.end())
                    {
                        size_t new_latency = node_global_latency + get_outputs(node)[output_port].latency;
                        max_latency = std::max(max_latency, new_latency);
                        input_global_latencies[it->second] = new_latency;
                    }
                }
            };

            for (size_t node_i = 0; node_i < _nodes.size() + 1; ++node_i) {
                if (node_i < _nodes.size())
                {
                    process_node(_nodes[node_i], node_i);
                }
                else
                {
                    process_node(*this, GRAPH_ID);
                }
            }
            return max_latency;
        }
    };

    struct alignas(max_align_t) AlignedBytes {
        std::byte b[alignof(max_align_t)];
    };

    class NodeProcessor {
        using Buffer = std::vector<AlignedBytes>;

        std::vector<std::shared_ptr<void>> _module_refs;
        TypeErasedNode _node;
        Buffer _buffer;
        NodeState _graph_state;
        InitBufferContext _counting_context;
#ifndef NDEBUG
        AllocationTrace _allocation_trace;
#endif

        void resize_buffer()
        {
            _buffer.reserve(1);
            std::byte* byte_data = reinterpret_cast<std::byte*>(static_cast<uintptr_t>(0x10000));
#ifndef NDEBUG
            _allocation_trace = {};
            CountingNonAllocator counter(byte_data, &_allocation_trace, byte_data);
#else
            CountingNonAllocator counter(byte_data);
#endif
            InitBufferContext ctx(InitBufferContext::PassMode::counting, counter.get_buffer());
            do_init_buffer(_node, counter, ctx);
            ctx.validate_after_counting();
            _buffer.resize(counter.estimate_buffer_size());
            _counting_context = std::move(ctx);
        }

        void initialize_graph()
        {
#ifndef NDEBUG
            FixedBufferAllocator allocator {
                {
                    reinterpret_cast<std::byte*>(_buffer.data()),
                    _buffer.size() * sizeof(AlignedBytes),
                },
                &_allocation_trace,
                reinterpret_cast<std::byte*>(_buffer.data()),
            };
            _allocation_trace.reset_replay();
#else
            FixedBufferAllocator allocator {
                {
                    reinterpret_cast<std::byte*>(_buffer.data()),
                    _buffer.size() * sizeof(AlignedBytes),
                },
            };
#endif
            InitBufferContext ctx = _counting_context.make_initializing_context(allocator.get_buffer());
            auto allocated = do_init_buffer(_node, allocator, ctx);
            ctx.validate_after_initialization();
            allocator.validate_trace_consumed();
            assert(
                _buffer.size() - allocated.size() < alignof(max_align_t) &&
                "buffer was over allocated"
            );
        }

    public:
        explicit NodeProcessor(
            TypeErasedNode node,
            std::vector<std::shared_ptr<void>> module_refs = {}
        )
        : _module_refs(std::move(module_refs))
        , _node(std::move(node))
        {
            assert(get_num_inputs(_node) == 0 && "the graph should have 0 inputs");
            assert(get_num_outputs(_node) == 0 && "the graph should have 0 outputs");

            resize_buffer();
            initialize_graph();
        }

        void tick(std::span<MidiMessage const> midi, size_t index)
        {
            std::span<std::byte> buffer_span{
                reinterpret_cast<std::byte*>(_buffer.data()),
                _buffer.size() * sizeof(AlignedBytes)
            };
            _node.tick({ NodeState { .inputs = {}, .outputs = {}, .buffer = buffer_span }, midi, index });
        }

        size_t get_latency() const
        {
            return _node.internal_latency();
        }

        size_t num_module_refs() const
        {
            return _module_refs.size();
        }
    };
}
