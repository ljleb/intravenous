#pragma once
#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/node_refs.h>
#include <intravenous/graph/builder/topology.h>
#include <intravenous/graph/builder/public_ports.h>
#include <intravenous/graph/builder/subgraphs.h>
#include <intravenous/graph/builder/connections.h>
#include <intravenous/graph/builder/detach.h>
#include <intravenous/graph/builder/annotations.h>
#include <intravenous/graph/builder/finalize.h>
#include <intravenous/basic_nodes/arithmetic.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cxxabi.h>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace iv {
    class GraphBuilder;

    class GraphBuilder {
        template<class Derived, class Node>
        friend class NodeRefBase;
        friend struct SamplePortRef;
        friend struct EventPortRef;
        friend class GraphBuilderChildEmbedder;
        friend class GraphBuilderConnections;
        friend class GraphBuilderDetach;
        friend class GraphBuilderAnnotations;

        GraphBuilderIdentity _identity;
        GraphBuilderTopology _topology;
        GraphBuilderConnections _connections;
        GraphBuilderPublicPorts _public_ports;
        SubgraphScopeManager _subgraphs;
        GraphBuilderDetach _detach;
        GraphBuilderAnnotations _annotations;

        explicit GraphBuilder(GraphBuilderIdentity identity);

    public:
        GraphBuilder();

        GraphBuilder derive_nested_builder();
        bool inside_subgraph_scope() const;
        ScopedSubgraph& current_scope();
        void define_scope_outputs(std::span<OutputRefConfig const> refs);
        void define_scope_event_outputs(std::span<EventOutputRefConfig const> refs);
        std::string node_id(size_t index) const;
        SamplePortRef input();
        SamplePortRef input(std::string_view name, Sample default_value = 0.0);
        EventPortRef event_input(std::string_view name, EventTypeId type);
        EventPortRef event_input(EventTypeId type);

        template<class Config>
        static void validate_output_port_configs(
            std::span<Config const> configs,
            std::string_view node_label,
            std::string_view kind
        );

        template<class Node, class... Args>
        details::node_ref_for_t<Node> node(Args&&... args);

        NodeRef embed_subgraph(GraphBuilder const& child);

        template<class... Refs>
        void event_outputs(Refs&&... refs);

        void event_outputs(std::span<EventOutputRefConfig const> refs);

        template<class Fn>
        NodeRef subgraph(Fn&& fn, std::string_view kind = "Subgraph");

        template<class... Refs>
        requires details::valid_node_call_args_v<Refs...>
        void outputs(Refs&&... refs);

        void outputs(std::initializer_list<NamedRef> refs);
        void outputs(std::span<OutputRefConfig const> refs);
        void outputs(std::span<NamedRef const> refs);

        using VacantSampleInput = GraphBuilderVacantSampleInput;
        using VacantEventInput = GraphBuilderVacantEventInput;
        using RootNodeBuildResult = GraphBuilderRootNodeBuildResult;
        using VacantInputs = GraphBuilderVacantInputs;
        using LogicalSampleInput = GraphBuilderLogicalSampleInput;
        using LogicalEventInput = GraphBuilderLogicalEventInput;
        using LogicalInputs = GraphBuilderLogicalInputs;

        VacantInputs vacant_inputs() const;
        LogicalInputs logical_inputs() const;
        void connect_sample_input(PortId target, SamplePortRef source);
        void connect_event_input(PortId target, EventPortRef source);
        void mark_runtime_filled_sample_input(PortId target);
        void mark_runtime_filled_event_input(PortId target);
        GraphIntrospectionMetadata build_metadata(size_t detach_id_offset = 0) const;
        RootNodeBuildResult build_root_node(size_t detach_id_offset = 0) const;

    private:
        static std::string allocate_root_builder_id();
        SamplePortRef detach_sample_port(SamplePortRef const& sample_port, size_t loop_extra_latency);
        SamplePortRef lift_to_sample_port(SamplePortRef const& sample_port);
        SamplePortRef lift_to_sample_port(SamplePortRef&& sample_port);

        template<class T>
        requires std::is_arithmetic_v<std::remove_cvref_t<T>> || std::is_same_v<std::remove_cvref_t<T>, Sample>
        SamplePortRef lift_to_sample_port(T value)
        {
            return node<Constant>(static_cast<Sample>(value));
        }

        SamplePortRef lift_to_sample_port(NamedRef const& ref);

    };

    template<class Config>
    void GraphBuilder::validate_output_port_configs(
        std::span<Config const> configs,
        std::string_view node_label,
        std::string_view kind
    )
    {
        GraphBuilderTopology::validate_output_port_configs(configs, node_label, kind);
    }

    template<class Node, class... Args>
    details::node_ref_for_t<Node> GraphBuilder::node(Args&&... args)
    {
        return _topology.insert_node<Node>(*this, std::forward<Args>(args)...);
    }

    template<class... Refs>
    void GraphBuilder::event_outputs(Refs&&... refs)
    {
        if (inside_subgraph_scope()) {
            std::vector<EventOutputRefConfig> output_refs;
            output_refs.reserve(sizeof...(Refs));
            constexpr bool require_names = (sizeof...(Refs) > 1);
            auto const append_ref = [&](auto&& ref) {
                using RefT = std::remove_cvref_t<decltype(ref)>;
                if constexpr (details::is_named_arg_v<RefT>) {
                    output_refs.push_back(EventOutputRefConfig{
                        .ref = static_cast<EventPortRef>(ref.value),
                        .config = EventOutputConfig{ .name = std::string(RefT::name.view()) },
                    });
                } else {
                    if constexpr (require_names) {
                        details::error(
                            "builder " + _identity.value
                            + ": event_outputs(...) requires names when exposing more than one event output"
                        );
                    } else {
                        output_refs.push_back(EventOutputRefConfig{
                            .ref = static_cast<EventPortRef>(ref),
                            .config = EventOutputConfig{},
                        });
                    }
                }
            };
            (append_ref(std::forward<Refs>(refs)), ...);
            define_scope_event_outputs(std::span<EventOutputRefConfig const>(output_refs.data(), output_refs.size()));
            return;
        }
        _public_ports.define_event_outputs_from_args(*this, _topology, _identity, std::forward<Refs>(refs)...);
    }

    template<class Fn>
    NodeRef GraphBuilder::subgraph(Fn&& fn, std::string_view kind)
    {
        return _subgraphs.run(*this, _topology, std::forward<Fn>(fn), kind);
    }

    template<class... Refs>
    requires details::valid_node_call_args_v<Refs...>
    void GraphBuilder::outputs(Refs&&... refs)
    {
        if (inside_subgraph_scope()) {
            std::vector<OutputRefConfig> output_refs;
            output_refs.reserve(sizeof...(Refs));
            constexpr bool require_names = (sizeof...(Refs) > 1);
            auto const append_ref = [&](auto&& ref) {
                using RefT = std::remove_cvref_t<decltype(ref)>;
                if constexpr (details::is_named_arg_v<RefT>) {
                    output_refs.push_back(OutputRefConfig{
                        .ref = lift_to_sample_port(ref.value),
                        .config = OutputConfig{ .name = std::string(RefT::name.view()) },
                    });
                } else {
                    if constexpr (require_names) {
                        details::error(
                            "builder " + _identity.value
                            + ": outputs(...) requires names when exposing more than one sample output"
                        );
                    } else {
                        output_refs.push_back(OutputRefConfig{
                            .ref = lift_to_sample_port(std::forward<decltype(ref)>(ref)),
                            .config = OutputConfig{},
                        });
                    }
                }
            };
            (append_ref(std::forward<Refs>(refs)), ...);
            define_scope_outputs(std::span<OutputRefConfig const>(output_refs.data(), output_refs.size()));
            return;
        }
        _public_ports.define_sample_outputs_from_args(
            *this,
            _topology,
            _identity,
            [&](auto&& value) {
                return lift_to_sample_port(std::forward<decltype(value)>(value));
            },
            std::forward<Refs>(refs)...
        );
    }

    template<class Derived, class Node>
    inline BuilderNode const& NodeRefBase<Derived, Node>::node() const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        return _graph_builder->_topology.node(_index);
    }

    template<class Derived, class Node>
    inline NodeRef NodeRefBase<Derived, Node>::node_ref() const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        return NodeRef(*_graph_builder, _index);
    }

    template<class Derived, class Node>
    inline Derived NodeRefBase<Derived, Node>::_clone_handle() const
    {
        if (!_graph_builder) {
            return Derived {};
        }
        return Derived(*_graph_builder, _index);
    }

    template<class Derived, class Node>
    inline Derived& NodeRefBase<Derived, Node>::operator=(Derived const& rhs)
    {
        if (this == &rhs) {
            return derived();
        }

        if (_allows_single_assignment) {
            if (!rhs._graph_builder) {
                details::error("cannot initialize a logical-empty NodeRef from an empty NodeRef");
            }
            _graph_builder = rhs._graph_builder;
            _index = rhs._index;
            if (!_logical_declaration_id.empty() && _graph_builder) {
                _graph_builder->_annotations.add_logical_id(_graph_builder->_topology, _index, _logical_declaration_id);
                _graph_builder->_annotations.initialize_vacant_input_owner(
                    _graph_builder->_topology,
                    _index,
                    _logical_declaration_id
                );
            }
            _allows_single_assignment = false;
            return derived();
        }

        if (_graph_builder || !_logical_declaration_id.empty()) {
            details::error(
                "cannot assign to NodeRef '" + _logical_declaration_id + "' after it has already been initialized"
            );
        }

        _graph_builder = rhs._graph_builder;
        _index = rhs._index;
        _logical_declaration_id = rhs._logical_declaration_id;
        _allows_single_assignment = rhs._allows_single_assignment;
        return Derived(*_graph_builder, _index);
    }

    template<class Derived, class Node>
    inline Derived& NodeRefBase<Derived, Node>::operator=(Derived&& rhs)
    {
        if (this == &rhs) {
            return derived();
        }

        if (_allows_single_assignment) {
            if (!rhs._graph_builder) {
                details::error("cannot initialize a logical-empty NodeRef from an empty NodeRef");
            }
            _graph_builder = rhs._graph_builder;
            _index = rhs._index;
            if (!_logical_declaration_id.empty() && _graph_builder) {
                _graph_builder->_annotations.add_logical_id(_graph_builder->_topology, _index, _logical_declaration_id);
                _graph_builder->_annotations.transfer_vacant_input_owner(
                    _graph_builder->_topology,
                    _index,
                    _logical_declaration_id
                );
            }
            _allows_single_assignment = false;
            rhs._graph_builder = nullptr;
            rhs._index = 0;
            rhs._logical_declaration_id.clear();
            rhs._allows_single_assignment = false;
            return derived();
        }

        if (_graph_builder || !_logical_declaration_id.empty()) {
            details::error(
                "cannot assign to NodeRef '" + _logical_declaration_id + "' after it has already been initialized"
            );
        }

        _graph_builder = rhs._graph_builder;
        _index = rhs._index;
        _logical_declaration_id = std::move(rhs._logical_declaration_id);
        _allows_single_assignment = rhs._allows_single_assignment;
        rhs._graph_builder = nullptr;
        rhs._index = 0;
        rhs._logical_declaration_id.clear();
        rhs._allows_single_assignment = false;
        return derived();
    }

    template<class Derived, class Node>
    inline void NodeRefBase<Derived, Node>::_annotate_source_info(
        std::string_view declaration_identity,
        std::string_view file_path,
        uint32_t begin,
        uint32_t end
    ) const
    {
        if (!declaration_identity.empty()) {
            _logical_declaration_id = declaration_identity;
        }
        if (!_graph_builder) {
            return;
        }
        auto const ref = NodeRef(*_graph_builder, _index);
        _graph_builder->_annotations.annotate_node_source_info(
            _graph_builder->_topology,
            _graph_builder->_identity,
            *_graph_builder,
            ref,
            declaration_identity,
            file_path,
            begin,
            end
        );
    }

    template<class Derived, class Node>
    inline SamplePortRef NodeRefBase<Derived, Node>::operator[](size_t output_index) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        return SamplePortRef(*_graph_builder, _index, output_index);
    }

    template<class Derived, class Node>
    inline SamplePortRef NodeRefBase<Derived, Node>::operator[](std::string_view output_name) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        auto outputs = get_outputs(node());
        std::optional<size_t> matched_output;
        for (size_t output_port = 0; output_port < outputs.size(); ++output_port) {
            if (outputs[output_port].name == output_name) {
                if (matched_output.has_value()) {
                    details::error(
                        "output name '" + std::string(output_name) + "' is ambiguous on " + to_string()
                    );
                }
                matched_output = output_port;
            }
        }

        if (matched_output.has_value()) {
            return SamplePortRef(*_graph_builder, _index, *matched_output);
        }

        details::error(
            "an output port named '" + std::string(output_name) + "' "
            "does not exist on " + to_string()
        );
    }

    template<class Derived, class Node>
    inline EventPortRef NodeRefBase<Derived, Node>::event_port(size_t output_index) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }

        auto outputs = get_event_outputs(node());
        if (output_index >= outputs.size()) {
            details::error(
                "event output port " + std::to_string(output_index) + " of "
                + to_string() + " is out of bounds"
            );
        }
        return EventPortRef(*_graph_builder, _index, output_index);
    }

    template<class Derived, class Node>
    inline EventPortRef NodeRefBase<Derived, Node>::event_port(std::string_view output_name) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }

        auto outputs = get_event_outputs(node());
        std::optional<size_t> matched_output;
        for (size_t output_port = 0; output_port < outputs.size(); ++output_port) {
            if (outputs[output_port].name == output_name) {
                if (matched_output.has_value()) {
                    details::error(
                        "event output name '" + std::string(output_name) + "' is ambiguous on " + to_string()
                    );
                }
                matched_output = output_port;
            }
        }

        if (matched_output.has_value()) {
            return EventPortRef(*_graph_builder, _index, *matched_output);
        }

        details::error(
            "an event output port named '" + std::string(output_name) + "' "
            "does not exist on " + to_string()
        );
    }

    template<class Derived, class Node>
    inline EventPortRef NodeRefBase<Derived, Node>::event_port() const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }

        auto outputs = get_event_outputs(node());
        if (outputs.size() != 1) {
            details::error(
                to_string() + " "
                "does not have exactly 1 event output port"
            );
        }
        return event_port(0);
    }

    template<class Derived, class Node>
    inline NodeRefBase<Derived, Node>::operator SamplePortRef() const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        if (get_num_outputs(node()) != 1) {
            details::error(
                to_string() + " "
                "does not have exactly 1 output port: it cannot be implicitly converted to SamplePortRef"
            );
        }
        return SamplePortRef(*_graph_builder, _index, 0);
    }

    template<class Derived, class Node>
    inline bool NodeRefBase<Derived, Node>::input_is_connected(size_t input_port) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        return _graph_builder->_connections.sample_input_is_connected(PortId{ _index, input_port });
    }

    template<class Derived, class Node>
    inline bool NodeRefBase<Derived, Node>::event_input_is_connected(size_t input_port) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        return _graph_builder->_connections.event_input_is_connected(PortId{ _index, input_port });
    }

    template<class Derived, class Node>
    template<class... Args>
    requires(details::node_call_enabled<Node, Args...>)
    inline Derived NodeRefBase<Derived, Node>::operator()(Args&&... args) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }

        auto const inputs = get_inputs(node());
        auto const event_inputs = get_event_inputs(node());

        auto connect_input = [&](SamplePortRef const& ref, size_t input_port) {
            if (input_port >= inputs.size()) {
                details::error(
                    to_string() + " "
                    "has at most " + std::to_string(inputs.size()) + " inputs, "
                    "got " + std::to_string(input_port + 1)
                );
            }

            if (ref.graph_builder != _graph_builder) {
                details::error(
                    ref.to_string() + " "
                    "does not belong to the same builder as " + to_string()
                );
            }

            _graph_builder->connect_sample_input(PortId{ _index, input_port }, ref);
        };

        auto connect_event = [&](EventPortRef ref, size_t input_port) {
            if (input_port >= event_inputs.size()) {
                details::error(
                    to_string() + " "
                    "has at most " + std::to_string(event_inputs.size()) + " event inputs, "
                    "got " + std::to_string(input_port + 1)
                );
            }
            if (ref.graph_builder != _graph_builder) {
                details::error(
                    ref.to_string() + " "
                    "does not belong to the same builder as " + to_string()
                );
            }

            _graph_builder->connect_event_input(PortId{ _index, input_port }, ref);
        };

        auto connect_named = [&](auto&& named_arg) {
            using NamedArgT = std::remove_cvref_t<decltype(named_arg)>;
            constexpr std::string_view name = NamedArgT::name.view();

            if constexpr (NamedArgT::kind == NamedPortKind::event) {
                for (size_t input_port = 0; input_port < event_inputs.size(); ++input_port) {
                    if (event_inputs[input_port].name == name) {
                        connect_event(lift_event_operand(named_arg.value), input_port);
                        return;
                    }
                }
            } else {
                for (size_t input_port = 0; input_port < inputs.size(); ++input_port) {
                    if (inputs[input_port].name == name) {
                        connect_input(_graph_builder->lift_to_sample_port(named_arg.value), input_port);
                        return;
                    }
                }
            }

            details::error(
                "an input port named '" + std::string(name) + "' "
                "does not exist on " + to_string()
            );
        };

        size_t positional_sample_port = 0;
        size_t positional_event_port = 0;
        auto const process_arg = [&](auto&& arg) {
            if constexpr (details::is_named_arg_v<decltype(arg)>) {
                connect_named(std::forward<decltype(arg)>(arg));
            } else if constexpr (details::graph_builder_event_port_like<decltype(arg)>) {
                connect_event(static_cast<EventPortRef>(std::forward<decltype(arg)>(arg)), positional_event_port++);
            } else {
                connect_input(
                    _graph_builder->lift_to_sample_port(std::forward<decltype(arg)>(arg)),
                    positional_sample_port++
                );
            }
        };

        (process_arg(std::forward<Args>(args)), ...);

        return Derived(*_graph_builder, _index);
    }

    template<class Derived, class Node>
    template<class T>
    inline Derived NodeRefBase<Derived, Node>::connect_input(size_t input_port, T&& value) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }

        auto const inputs = get_inputs(node());
        if (input_port >= inputs.size()) {
            details::error(
                to_string() + " "
                "has at most " + std::to_string(inputs.size()) + " inputs, "
                "got " + std::to_string(input_port + 1)
            );
        }

        SamplePortRef ref = _graph_builder->lift_to_sample_port(std::forward<T>(value));
        if (ref.graph_builder != _graph_builder) {
            details::error(
                ref.to_string() + " "
                "does not belong to the same builder as " + to_string()
            );
        }

        _graph_builder->connect_sample_input(PortId{ _index, input_port }, ref);
        return Derived(*_graph_builder, _index);
    }

    template<class Derived, class Node>
    template<class T>
    inline Derived NodeRefBase<Derived, Node>::connect_input(std::string_view input_name, T&& value) const
    {
        auto const inputs = get_inputs(node());
        std::optional<size_t> matched_input;
        for (size_t input_port = 0; input_port < inputs.size(); ++input_port) {
            if (inputs[input_port].name == input_name) {
                if (matched_input.has_value()) {
                    details::error(
                        "input name '" + std::string(input_name) + "' is ambiguous on " + to_string()
                    );
                }
                matched_input = input_port;
            }
        }

        if (matched_input.has_value()) {
            return connect_input(*matched_input, std::forward<T>(value));
        }

        details::error(
            "an input port named '" + std::string(input_name) + "' "
            "does not exist on " + to_string()
        );
    }

    template<class Derived, class Node>
    inline Derived NodeRefBase<Derived, Node>::connect_event_input(size_t input_port, EventPortRef value) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }

        auto const event_inputs = get_event_inputs(node());
        if (input_port >= event_inputs.size()) {
            details::error(
                to_string() + " "
                "has at most " + std::to_string(event_inputs.size()) + " event inputs, "
                "got " + std::to_string(input_port + 1)
            );
        }
        if (value.graph_builder != _graph_builder) {
            details::error(
                value.to_string() + " "
                "does not belong to the same builder as " + to_string()
            );
        }

        _graph_builder->connect_event_input(PortId{ _index, input_port }, value);
        return Derived(*_graph_builder, _index);
    }

    template<class Derived, class Node>
    inline Derived NodeRefBase<Derived, Node>::connect_event_input(std::string_view input_name, EventPortRef value) const
    {
        auto const event_inputs = get_event_inputs(node());
        std::optional<size_t> matched_input;
        for (size_t input_port = 0; input_port < event_inputs.size(); ++input_port) {
            if (event_inputs[input_port].name == input_name) {
                if (matched_input.has_value()) {
                    details::error(
                        "event input name '" + std::string(input_name) + "' is ambiguous on " + to_string()
                    );
                }
                matched_input = input_port;
            }
        }

        if (matched_input.has_value()) {
            return connect_event_input(*matched_input, value);
        }

        details::error(
            "an event input port named '" + std::string(input_name) + "' "
            "does not exist on " + to_string()
        );
    }

    template<class Derived, class Node>
    inline SamplePortRef NodeRefBase<Derived, Node>::detach(size_t loop_extra_latency) const
    {
        return static_cast<SamplePortRef>(*this).detach(loop_extra_latency);
    }

    template<class Derived, class Node>
    inline Derived NodeRefBase<Derived, Node>::ttl(size_t ttl_samples) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        _graph_builder->_topology.apply_ttl(_index, ttl_samples);
        return Derived(*_graph_builder, _index);
    }

    template<class Derived, class Node>
    inline Derived NodeRefBase<Derived, Node>::no_ttl() const
    {
        return ttl(std::numeric_limits<size_t>::max());
    }

    template<class Derived, class Node>
    inline std::string NodeRefBase<Derived, Node>::to_string() const
    {
        if (!_graph_builder) {
            return "empty node";
        }
        return "node at address " + _graph_builder->node_id(_index);
    }

}
