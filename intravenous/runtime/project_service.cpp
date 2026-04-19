#include "runtime/project_service.h"

#include "runtime/config.h"
#include "compat.h"
#include "devices/miniaudio_device.h"
#include "graph/node.h"
#include "juce/vst_runtime.h"
#include "module/loader.h"
#include "module/search_paths.h"
#include "module/watcher.h"
#include "node/executor.h"
#include "orchestrator/orchestrator_builder.h"
#include "runtime/handlers.h"
#include "runtime/reload_worker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cxxabi.h>
#include <fstream>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <thread>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace iv {
    namespace {
        struct LineIndex {
            std::string text;
            std::vector<size_t> line_offsets;

            static LineIndex from_file(std::filesystem::path const& path)
            {
                std::ifstream in(path, std::ios::binary);
                if (!in) {
                    throw std::runtime_error("failed to open source file: " + path.string());
                }

                LineIndex index;
                index.text.assign(
                    std::istreambuf_iterator<char>(in),
                    std::istreambuf_iterator<char>()
                );
                index.line_offsets.push_back(0);
                for (size_t i = 0; i < index.text.size(); ++i) {
                    if (index.text[i] == '\n') {
                        index.line_offsets.push_back(i + 1);
                    }
                }
                return index;
            }

            size_t offset_for(SourcePosition position) const
            {
                if (line_offsets.empty()) {
                    return 0;
                }

                size_t const line_index = position.line <= 1
                    ? 0
                    : std::min<size_t>(position.line - 1, line_offsets.size() - 1);
                size_t const line_start = line_offsets[line_index];
                size_t const next_line_start = line_index + 1 < line_offsets.size()
                    ? line_offsets[line_index + 1]
                    : text.size();
                size_t const requested_column = position.column <= 1 ? 0 : position.column - 1;
                return std::min(line_start + requested_column, next_line_start);
            }

            SourcePosition position_for(size_t offset) const
            {
                offset = std::min(offset, text.size());
                auto const it = std::upper_bound(line_offsets.begin(), line_offsets.end(), offset);
                size_t const line_index = it == line_offsets.begin() ? 0 : static_cast<size_t>(std::distance(line_offsets.begin(), it - 1));
                return SourcePosition {
                    .line = static_cast<uint32_t>(line_index + 1),
                    .column = static_cast<uint32_t>(offset - line_offsets[line_index] + 1),
                };
            }
        };

        struct SnapshotNodeSpan {
            std::string file_path;
            uint32_t begin = 0;
            uint32_t end = 0;

            bool operator==(SnapshotNodeSpan const&) const = default;
        };

        struct SnapshotSourceInfo {
            std::string declaration_identity;
            SnapshotNodeSpan span;

            bool operator==(SnapshotSourceInfo const&) const = default;
        };

        struct ConcretePortInfo {
            std::string name;
            std::string type;
            bool connected = false;
            size_t history = 0;
            size_t latency = 0;
            Sample default_value = 0.0f;
        };

        struct ConcreteNode {
            std::string id;
            std::string kind;
            std::vector<SnapshotSourceInfo> source_infos;
            std::vector<ConcretePortInfo> sample_inputs;
            std::vector<ConcretePortInfo> sample_outputs;
            std::vector<ConcretePortInfo> event_inputs;
            std::vector<ConcretePortInfo> event_outputs;
        };

        struct LogicalSnapshotNode {
            std::string id;
            std::string kind;
            std::vector<SnapshotNodeSpan> source_spans;
            std::string source_declaration_identity;
            std::vector<LogicalPortInfo> sample_inputs;
            std::vector<LogicalPortInfo> sample_outputs;
            std::vector<LogicalPortInfo> event_inputs;
            std::vector<LogicalPortInfo> event_outputs;
            std::vector<std::string> backing_node_ids;
            std::string stable_identity_key;
        };

        struct GraphSnapshot {
            uint64_t execution_epoch = 0;
            std::filesystem::path module_root;
            std::string module_id;
            std::vector<ConcreteNode> concrete_nodes;
            std::vector<LogicalSnapshotNode> logical_nodes;
            std::unordered_map<std::string, size_t> logical_node_index_by_id;
        };

        std::filesystem::path normalize_path(std::filesystem::path const& path)
        {
            return std::filesystem::weakly_canonical(path).lexically_normal();
        }

        std::string normalized_path_string(std::filesystem::path const& path)
        {
            return normalize_path(path).generic_string();
        }

        std::optional<LogicalAudioDevice> make_default_audio_device()
        {
            return make_miniaudio_device({});
        }

        ModuleRenderConfig module_render_config(RenderConfig const& config)
        {
            return ModuleRenderConfig {
                .sample_rate = config.sample_rate,
                .num_channels = config.num_channels,
                .max_block_frames = config.max_block_frames,
            };
        }

        DeviceOrchestrator make_audio_device_provider(LogicalAudioDevice audio_device)
        {
            std::vector<OutputDeviceMixer> mixers;
            mixers.emplace_back(
                std::move(audio_device),
                [](OrchestratorBuilder& builder, OutputDeviceMixer&& mixer) {
                    builder.add_audio_mixer(0, std::move(mixer));
                }
            );
            return DeviceOrchestrator(std::move(mixers));
        }

        ResourceContext make_resource_context(RenderConfig const& render_config)
        {
            ResourceContext resources {};
#if IV_ENABLE_JUCE_VST
            static JuceVstRuntimeManager juce_vst_runtime_manager;
            static std::unique_ptr<JuceVstRuntimeSupport> juce_vst_runtime_support;
            juce_vst_runtime_support = std::make_unique<JuceVstRuntimeSupport>(
                juce_vst_runtime_manager,
                static_cast<double>(render_config.sample_rate)
            );
            resources = juce_vst_runtime_support->resources();
#endif
            return resources;
        }

        NodeExecutor make_executor(
            RenderConfig const& render_config,
            DeviceOrchestrator& device_orchestrator,
            ModuleLoader::LoadedGraph loaded_graph
        )
        {
            return NodeExecutor::create(
                std::move(loaded_graph.root),
                make_resource_context(render_config),
                std::move(device_orchestrator).to_builder(),
                std::move(loaded_graph.module_refs)
            );
        }

        std::string demangle_type_name(char const* name)
        {
            if (name == nullptr || *name == '\0') {
                return {};
            }
#if defined(__GNUG__)
            int status = 0;
            std::unique_ptr<char, void(*)(void*)> demangled(
                abi::__cxa_demangle(name, nullptr, nullptr, &status),
                std::free
            );
            if (status == 0 && demangled) {
                return demangled.get();
            }
#endif
            return name;
        }

        std::string event_type_name(EventTypeId type)
        {
            switch (type) {
            case EventTypeId::midi:
                return "midi";
            case EventTypeId::trigger:
                return "trigger";
            case EventTypeId::boundary:
                return "boundary";
            case EventTypeId::empty:
                return "empty";
            case EventTypeId::count:
                break;
            }
            return "unknown";
        }

        LogicalPortConnectivity aggregate_connectivity(std::span<ConcretePortInfo const> ports)
        {
            bool any_connected = false;
            bool any_disconnected = false;
            for (auto const& port : ports) {
                any_connected = any_connected || port.connected;
                any_disconnected = any_disconnected || !port.connected;
            }
            if (any_connected && any_disconnected) {
                return LogicalPortConnectivity::mixed;
            }
            return any_connected
                ? LogicalPortConnectivity::connected
                : LogicalPortConnectivity::disconnected;
        }

        bool sample_input_connected(
            std::unordered_set<GraphEdge> const& edges,
            std::unordered_set<size_t> const& member_set,
            std::span<PortId const> targets
        )
        {
            return std::ranges::any_of(edges, [&](GraphEdge const& edge) {
                return std::ranges::any_of(targets, [&](PortId const& target) {
                    return edge.target == target
                        && (edge.source.node == GRAPH_ID || !member_set.contains(edge.source.node));
                });
            });
        }

        bool sample_output_connected(
            std::unordered_set<GraphEdge> const& edges,
            std::unordered_set<size_t> const& member_set,
            PortId source
        )
        {
            return std::ranges::any_of(edges, [&](GraphEdge const& edge) {
                return edge.source == source
                    && (edge.target.node == GRAPH_ID || !member_set.contains(edge.target.node));
            });
        }

        bool event_input_connected(
            std::unordered_set<GraphEventEdge> const& edges,
            std::unordered_set<size_t> const& member_set,
            std::span<PortId const> targets
        )
        {
            return std::ranges::any_of(edges, [&](GraphEventEdge const& edge) {
                return std::ranges::any_of(targets, [&](PortId const& target) {
                    return edge.target == target
                        && (edge.source.node == GRAPH_ID || !member_set.contains(edge.source.node));
                });
            });
        }

        bool event_output_connected(
            std::unordered_set<GraphEventEdge> const& edges,
            std::unordered_set<size_t> const& member_set,
            PortId source
        )
        {
            return std::ranges::any_of(edges, [&](GraphEventEdge const& edge) {
                return edge.source == source
                    && (edge.target.node == GRAPH_ID || !member_set.contains(edge.target.node));
            });
        }

        bool same_port_schema(
            std::span<ConcretePortInfo const> a,
            std::span<ConcretePortInfo const> b
        )
        {
            if (a.size() != b.size()) {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i) {
                if (a[i].name != b[i].name
                    || a[i].type != b[i].type
                    || a[i].history != b[i].history
                    || a[i].latency != b[i].latency
                    || a[i].default_value != b[i].default_value) {
                    return false;
                }
            }
            return true;
        }

        bool same_logical_schema(ConcreteNode const& a, ConcreteNode const& b)
        {
            return a.kind == b.kind
                && same_port_schema(a.sample_inputs, b.sample_inputs)
                && same_port_schema(a.sample_outputs, b.sample_outputs)
                && same_port_schema(a.event_inputs, b.event_inputs)
                && same_port_schema(a.event_outputs, b.event_outputs);
        }

        void sort_and_deduplicate_spans(std::vector<SnapshotNodeSpan>& spans)
        {
            std::sort(spans.begin(), spans.end(), [](auto const& a, auto const& b) {
                return std::tie(a.file_path, a.begin, a.end) < std::tie(b.file_path, b.begin, b.end);
            });
            spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
        }

        std::vector<std::string> declaration_identities_for(ConcreteNode const& node)
        {
            std::vector<std::string> declaration_identities;
            declaration_identities.reserve(node.source_infos.size());
            for (auto const& info : node.source_infos) {
                if (!info.declaration_identity.empty()) {
                    declaration_identities.push_back(info.declaration_identity);
                }
            }
            std::sort(declaration_identities.begin(), declaration_identities.end());
            declaration_identities.erase(
                std::unique(declaration_identities.begin(), declaration_identities.end()),
                declaration_identities.end()
            );
            return declaration_identities;
        }

        std::vector<SnapshotNodeSpan> source_spans_for(
            std::span<ConcreteNode const* const> nodes,
            std::string_view declaration_identity
        )
        {
            std::vector<SnapshotNodeSpan> spans;
            for (auto const* node : nodes) {
                for (auto const& info : node->source_infos) {
                    if (info.declaration_identity != declaration_identity) {
                        continue;
                    }
                    if (info.span.file_path.empty() || info.span.begin > info.span.end) {
                        continue;
                    }
                    spans.push_back(info.span);
                }
            }
            sort_and_deduplicate_spans(spans);
            return spans;
        }

        std::vector<LogicalPortInfo> aggregate_ports(
            std::span<ConcreteNode const* const> nodes,
            auto ConcreteNode::* member
        )
        {
            if (nodes.empty()) {
                return {};
            }

            auto const& first_ports = nodes.front()->*member;
            std::vector<LogicalPortInfo> logical_ports;
            logical_ports.reserve(first_ports.size());
            for (size_t i = 0; i < first_ports.size(); ++i) {
                std::vector<ConcretePortInfo> concrete_ports;
                concrete_ports.reserve(nodes.size());
                for (auto const* node : nodes) {
                    concrete_ports.push_back((node->*member)[i]);
                }

                logical_ports.push_back(LogicalPortInfo {
                    .name = first_ports[i].name,
                    .type = first_ports[i].type,
                    .connectivity = aggregate_connectivity(concrete_ports),
                });
            }
            return logical_ports;
        }

        LogicalSnapshotNode make_logical_node_from_members(
            std::span<ConcreteNode const* const> members,
            std::vector<SnapshotNodeSpan> source_spans,
            std::vector<std::string> backing_node_ids,
            std::string declaration_identity
        )
        {
            std::sort(backing_node_ids.begin(), backing_node_ids.end());
            sort_and_deduplicate_spans(source_spans);
            return LogicalSnapshotNode {
                .id = {},
                .kind = members.empty() ? std::string{} : members.front()->kind,
                .source_spans = std::move(source_spans),
                .source_declaration_identity = std::move(declaration_identity),
                .sample_inputs = aggregate_ports(members, &ConcreteNode::sample_inputs),
                .sample_outputs = aggregate_ports(members, &ConcreteNode::sample_outputs),
                .event_inputs = aggregate_ports(members, &ConcreteNode::event_inputs),
                .event_outputs = aggregate_ports(members, &ConcreteNode::event_outputs),
                .backing_node_ids = std::move(backing_node_ids),
                .stable_identity_key = {},
            };
        }

        void append_port_schema_signature(std::ostringstream& key, std::span<ConcretePortInfo const> ports)
        {
            key << "[";
            for (size_t i = 0; i < ports.size(); ++i) {
                if (i != 0) {
                    key << ";";
                }
                key
                    << ports[i].name << ","
                    << ports[i].type << ","
                    << ports[i].history << ","
                    << ports[i].latency << ","
                    << ports[i].default_value;
            }
            key << "]";
        }

        std::string stable_identity_key_for(
            std::string_view declaration_identity,
            ConcreteNode const& representative
        )
        {
            if (declaration_identity.empty()) {
                return {};
            }
            std::ostringstream key;
            key << "decl=" << declaration_identity;
            key << "|kind=" << representative.kind;
            key << "|sample_inputs=";
            append_port_schema_signature(key, representative.sample_inputs);
            key << "|sample_outputs=";
            append_port_schema_signature(key, representative.sample_outputs);
            key << "|event_inputs=";
            append_port_schema_signature(key, representative.event_inputs);
            key << "|event_outputs=";
            append_port_schema_signature(key, representative.event_outputs);
            return key.str();
        }

        std::vector<LogicalSnapshotNode> build_logical_nodes(
            std::span<ConcreteNode const> concrete_nodes
        )
        {
            struct DeclarationGroup {
                std::string declaration_identity;
                std::vector<size_t> member_indices;
            };

            std::vector<DeclarationGroup> groups;
            for (size_t i = 0; i < concrete_nodes.size(); ++i) {
                for (auto const& declaration_identity : declaration_identities_for(concrete_nodes[i])) {
                    auto group_it = std::find_if(groups.begin(), groups.end(), [&](auto const& group) {
                        return group.declaration_identity == declaration_identity
                            && !group.member_indices.empty()
                            && same_logical_schema(concrete_nodes[group.member_indices.front()], concrete_nodes[i]);
                    });
                    if (group_it == groups.end()) {
                        groups.push_back(DeclarationGroup {
                            .declaration_identity = declaration_identity,
                            .member_indices = { i },
                        });
                    } else if (!std::ranges::contains(group_it->member_indices, i)) {
                        group_it->member_indices.push_back(i);
                    }
                }
            }

            std::vector<LogicalSnapshotNode> candidates;
            candidates.reserve(groups.size());
            for (auto const& group : groups) {
                std::vector<ConcreteNode const*> members;
                members.reserve(group.member_indices.size());
                std::vector<std::string> backing_node_ids;
                backing_node_ids.reserve(group.member_indices.size());
                for (auto member_index : group.member_indices) {
                    members.push_back(&concrete_nodes[member_index]);
                    backing_node_ids.push_back(concrete_nodes[member_index].id);
                }

                auto logical = make_logical_node_from_members(
                    members,
                    source_spans_for(members, group.declaration_identity),
                    std::move(backing_node_ids),
                    group.declaration_identity
                );
                logical.stable_identity_key = stable_identity_key_for(group.declaration_identity, *members.front());
                candidates.push_back(std::move(logical));
            }

            std::sort(candidates.begin(), candidates.end(), [](auto const& a, auto const& b) {
                auto const a_file = a.source_spans.empty() ? std::string{} : a.source_spans.front().file_path;
                auto const b_file = b.source_spans.empty() ? std::string{} : b.source_spans.front().file_path;
                if (a_file != b_file) {
                    return a_file < b_file;
                }
                auto const a_begin = a.source_spans.empty() ? 0u : a.source_spans.front().begin;
                auto const b_begin = b.source_spans.empty() ? 0u : b.source_spans.front().begin;
                if (a_begin != b_begin) {
                    return a_begin < b_begin;
                }
                auto const a_end = a.source_spans.empty() ? 0u : a.source_spans.front().end;
                auto const b_end = b.source_spans.empty() ? 0u : b.source_spans.front().end;
                if (a_end != b_end) {
                    return a_end < b_end;
                }
                if (a.kind != b.kind) {
                    return a.kind < b.kind;
                }
                if (a.source_declaration_identity != b.source_declaration_identity) {
                    return a.source_declaration_identity < b.source_declaration_identity;
                }
                return a.backing_node_ids < b.backing_node_ids;
            });

            return candidates;
        }

        void log_reload_exception(std::string_view context, std::exception_ptr exception)
        {
            if (!exception) {
                return;
            }

            auto& out = diagnostic_stream();
            out << context;
            try {
                std::rethrow_exception(exception);
            } catch (std::exception const& e) {
                out << ": " << e.what() << '\n';
            } catch (...) {
                out << ": unknown exception\n";
            }
            out.flush();
        }

        std::string describe_exception(std::exception_ptr exception)
        {
            if (!exception) {
                return "unknown exception";
            }

            try {
                std::rethrow_exception(exception);
            } catch (std::exception const& e) {
                return e.what();
            } catch (...) {
                return "unknown exception";
            }
        }

        Graph const& require_root_graph(TypeErasedNode const& root)
        {
            auto const* graph = root.try_as<Graph>();
            if (!graph) {
                throw std::runtime_error("root module did not lower to iv::Graph");
            }
            return *graph;
        }

        GraphSnapshot build_graph_snapshot(
            TypeErasedNode const& root,
            GraphIntrospectionMetadata const& introspection,
            std::filesystem::path const& module_root,
            std::string const& module_id,
            uint64_t execution_epoch
        )
        {
            Graph const& graph = require_root_graph(root);

            std::unordered_set<GraphEdge> const& sample_edges = graph._edges;
            std::unordered_set<GraphEventEdge> const& event_edges = graph._event_edges;

            auto execution_sample_input_connected = [&](size_t node, size_t port) {
                return std::ranges::any_of(sample_edges, [&](GraphEdge const& edge) {
                    return edge.target.node == node && edge.target.port == port;
                });
            };
            auto execution_sample_output_connected = [&](size_t node, size_t port) {
                return std::ranges::any_of(sample_edges, [&](GraphEdge const& edge) {
                    return edge.source.node == node && edge.source.port == port;
                });
            };
            auto execution_event_input_connected = [&](size_t node, size_t port) {
                return std::ranges::any_of(event_edges, [&](GraphEventEdge const& edge) {
                    return edge.target.node == node && edge.target.port == port;
                });
            };
            auto execution_event_output_connected = [&](size_t node, size_t port) {
                return std::ranges::any_of(event_edges, [&](GraphEventEdge const& edge) {
                    return edge.source.node == node && edge.source.port == port;
                });
            };

            std::vector<std::optional<ConcreteNode>> by_global_index(graph._node_ids.size());
            for (GraphSccWrapper const& scc : graph._scc_wrappers) {
                for (size_t local_i = 0; local_i < scc._nodes.size(); ++local_i) {
                    size_t const global_i = scc._global_node_indices[local_i];
                    GraphNodeWrapper const& node = scc._nodes[local_i];

                    ConcreteNode snapshot_node;
                    snapshot_node.id = graph._node_ids[global_i];
                    snapshot_node.kind = demangle_type_name(node._node.type_name());

                    if (global_i < introspection.node_source_infos.size()) {
                        for (SourceInfo const& info : introspection.node_source_infos[global_i]) {
                            snapshot_node.source_infos.push_back(SnapshotSourceInfo {
                                .declaration_identity = info.declaration_identity,
                                .span = SnapshotNodeSpan {
                                    .file_path = info.span.file_path.empty() ? std::string{} : normalized_path_string(info.span.file_path),
                                    .begin = info.span.begin,
                                    .end = info.span.end,
                                },
                            });
                        }
                    }

                    auto const inputs = node.inputs();
                    snapshot_node.sample_inputs.reserve(inputs.size());
                    for (size_t input_i = 0; input_i < inputs.size(); ++input_i) {
                        snapshot_node.sample_inputs.push_back(ConcretePortInfo {
                            .name = inputs[input_i].name,
                            .type = "sample",
                            .connected = execution_sample_input_connected(global_i, input_i),
                            .history = inputs[input_i].history,
                            .default_value = inputs[input_i].default_value,
                        });
                    }

                    auto const outputs = node.outputs();
                    snapshot_node.sample_outputs.reserve(outputs.size());
                    for (size_t output_i = 0; output_i < outputs.size(); ++output_i) {
                        snapshot_node.sample_outputs.push_back(ConcretePortInfo {
                            .name = outputs[output_i].name,
                            .type = "sample",
                            .connected = execution_sample_output_connected(global_i, output_i),
                            .history = outputs[output_i].history,
                            .latency = outputs[output_i].latency,
                        });
                    }

                    auto const event_inputs = node.event_inputs();
                    snapshot_node.event_inputs.reserve(event_inputs.size());
                    for (size_t input_i = 0; input_i < event_inputs.size(); ++input_i) {
                        snapshot_node.event_inputs.push_back(ConcretePortInfo {
                            .name = event_inputs[input_i].name,
                            .type = event_type_name(event_inputs[input_i].type),
                            .connected = execution_event_input_connected(global_i, input_i),
                        });
                    }

                    auto const event_outputs = node.event_outputs();
                    snapshot_node.event_outputs.reserve(event_outputs.size());
                    for (size_t output_i = 0; output_i < event_outputs.size(); ++output_i) {
                        snapshot_node.event_outputs.push_back(ConcretePortInfo {
                            .name = event_outputs[output_i].name,
                            .type = event_type_name(event_outputs[output_i].type),
                            .connected = execution_event_output_connected(global_i, output_i),
                        });
                    }

                    by_global_index[global_i] = std::move(snapshot_node);
                }
            }

            GraphSnapshot snapshot;
            snapshot.execution_epoch = execution_epoch;
            snapshot.module_root = normalize_path(module_root);
            snapshot.module_id = module_id;
            snapshot.concrete_nodes.reserve(by_global_index.size());
            for (auto& maybe_node : by_global_index) {
                if (!maybe_node.has_value()) {
                    continue;
                }
                snapshot.concrete_nodes.push_back(std::move(*maybe_node));
            }

            snapshot.concrete_nodes.reserve(snapshot.concrete_nodes.size() + introspection.lowered_subgraphs.size());
            for (size_t lowered_i = 0; lowered_i < introspection.lowered_subgraphs.size(); ++lowered_i) {
                auto const& lowered_subgraph = introspection.lowered_subgraphs[lowered_i];
                ConcreteNode snapshot_node;
                snapshot_node.id = "subgraph:" + std::to_string(lowered_i);
                snapshot_node.kind = lowered_subgraph.kind.empty() ? std::string("Subgraph") : lowered_subgraph.kind;

                for (auto const& span : lowered_subgraph.source_spans) {
                    snapshot_node.source_infos.push_back(SnapshotSourceInfo {
                        .declaration_identity = {},
                        .span = SnapshotNodeSpan {
                            .file_path = normalized_path_string(span.file_path),
                            .begin = span.begin,
                            .end = span.end,
                        },
                    });
                }

                std::unordered_set<size_t> member_set;
                member_set.reserve(lowered_subgraph.member_nodes.size());
                for (size_t member_node : lowered_subgraph.member_nodes) {
                    member_set.insert(member_node);
                }

                snapshot_node.sample_inputs.reserve(lowered_subgraph.sample_inputs.size());
                for (size_t input_i = 0; input_i < lowered_subgraph.sample_inputs.size(); ++input_i) {
                    auto const& input = lowered_subgraph.sample_inputs[input_i];
                    auto const targets = input_i < lowered_subgraph.sample_input_targets.size()
                        ? std::span<PortId const>(lowered_subgraph.sample_input_targets[input_i].data(), lowered_subgraph.sample_input_targets[input_i].size())
                        : std::span<PortId const>();
                    snapshot_node.sample_inputs.push_back(ConcretePortInfo {
                        .name = input.name,
                        .type = "sample",
                        .connected = sample_input_connected(sample_edges, member_set, targets),
                        .history = input.history,
                        .default_value = input.default_value,
                    });
                }

                snapshot_node.sample_outputs.reserve(lowered_subgraph.sample_outputs.size());
                for (size_t output_i = 0; output_i < lowered_subgraph.sample_outputs.size(); ++output_i) {
                    auto const& output = lowered_subgraph.sample_outputs[output_i];
                    PortId const source = output_i < lowered_subgraph.sample_output_sources.size()
                        ? lowered_subgraph.sample_output_sources[output_i]
                        : PortId{};
                    snapshot_node.sample_outputs.push_back(ConcretePortInfo {
                        .name = output.name,
                        .type = "sample",
                        .connected = sample_output_connected(sample_edges, member_set, source),
                        .latency = output.latency,
                    });
                }

                snapshot_node.event_inputs.reserve(lowered_subgraph.event_inputs.size());
                for (size_t input_i = 0; input_i < lowered_subgraph.event_inputs.size(); ++input_i) {
                    auto const& input = lowered_subgraph.event_inputs[input_i];
                    auto const targets = input_i < lowered_subgraph.event_input_targets.size()
                        ? std::span<PortId const>(lowered_subgraph.event_input_targets[input_i].data(), lowered_subgraph.event_input_targets[input_i].size())
                        : std::span<PortId const>();
                    snapshot_node.event_inputs.push_back(ConcretePortInfo {
                        .name = input.name,
                        .type = event_type_name(input.type),
                        .connected = event_input_connected(event_edges, member_set, targets),
                    });
                }

                snapshot_node.event_outputs.reserve(lowered_subgraph.event_outputs.size());
                for (size_t output_i = 0; output_i < lowered_subgraph.event_outputs.size(); ++output_i) {
                    auto const& output = lowered_subgraph.event_outputs[output_i];
                    PortId const source = output_i < lowered_subgraph.event_output_sources.size()
                        ? lowered_subgraph.event_output_sources[output_i]
                        : PortId{};
                    snapshot_node.event_outputs.push_back(ConcretePortInfo {
                        .name = output.name,
                        .type = event_type_name(output.type),
                        .connected = event_output_connected(event_edges, member_set, source),
                    });
                }

                snapshot.concrete_nodes.push_back(std::move(snapshot_node));
            }

            snapshot.logical_nodes = build_logical_nodes(snapshot.concrete_nodes);

            return snapshot;
        }
    }

    class RuntimeProjectService::Impl {
    public:
        std::filesystem::path workspace_root;
        std::filesystem::path discovery_start;
        std::vector<std::filesystem::path> extra_search_roots;
        AudioDeviceFactory audio_device_factory;
        RuntimeProjectEventSink event_sink;

        mutable std::mutex mutex;
        mutable std::unordered_map<std::string, LineIndex> line_index_cache;
        std::condition_variable initialized_cv;

        std::optional<RuntimeProjectConfig> config;
        std::optional<GraphSnapshot> snapshot;
        std::exception_ptr pending_exception;
        bool initialized = false;
        bool shutdown_requested = false;
        uint64_t next_logical_node_id = 1;

        std::optional<std::jthread> runtime_thread;
        NodeExecutor* executor_state = nullptr;

        explicit Impl(
            std::filesystem::path workspace_root_,
            std::filesystem::path discovery_start_,
            std::vector<std::filesystem::path> extra_search_roots_,
            AudioDeviceFactory audio_device_factory_,
            RuntimeProjectEventSink event_sink_
        ) :
            workspace_root(normalize_path(workspace_root_)),
            discovery_start(std::move(discovery_start_)),
            extra_search_roots(std::move(extra_search_roots_)),
            audio_device_factory(std::move(audio_device_factory_)),
            event_sink(std::move(event_sink_))
        {}

        std::string allocate_logical_node_id()
        {
            return "logical." + std::to_string(next_logical_node_id++);
        }

        std::vector<std::string> assign_logical_node_ids(
            GraphSnapshot& graph_snapshot,
            std::unordered_map<std::string, std::string> const& previous_ids_by_stable_key
        )
        {
            std::vector<std::string> created_node_ids;
            created_node_ids.reserve(graph_snapshot.logical_nodes.size());

            for (auto& node : graph_snapshot.logical_nodes) {
                if (!node.stable_identity_key.empty()) {
                    if (auto const it = previous_ids_by_stable_key.find(node.stable_identity_key); it != previous_ids_by_stable_key.end()) {
                        node.id = it->second;
                    } else {
                        node.id = allocate_logical_node_id();
                        created_node_ids.push_back(node.id);
                    }
                } else {
                    node.id = allocate_logical_node_id();
                    created_node_ids.push_back(node.id);
                }
            }

            graph_snapshot.logical_node_index_by_id.clear();
            for (size_t i = 0; i < graph_snapshot.logical_nodes.size(); ++i) {
                graph_snapshot.logical_node_index_by_id.emplace(graph_snapshot.logical_nodes[i].id, i);
            }

            return created_node_ids;
        }

        void emit_event(RuntimeProjectEvent event)
        {
            if (event.module_root.empty()) {
                if (config.has_value()) {
                    event.module_root = config->module_root;
                } else if (snapshot.has_value()) {
                    event.module_root = snapshot->module_root;
                }
            }
            if (event.execution_epoch == 0 && snapshot.has_value()) {
                event.execution_epoch = snapshot->execution_epoch;
            }
            if (event_sink) {
                event_sink(event);
            }
        }

        void emit_log(std::string level, std::string message)
        {
            emit_event(RuntimeProjectEvent {
                .kind = RuntimeProjectEventKind::log,
                .level = std::move(level),
                .message = std::move(message),
            });
        }

        void rethrow_if_failed() const
        {
            if (pending_exception) {
                std::rethrow_exception(pending_exception);
            }
        }

        LineIndex const& line_index_for(std::string const& normalized_path) const
        {
            auto it = line_index_cache.find(normalized_path);
            if (it == line_index_cache.end()) {
                it = line_index_cache.emplace(normalized_path, LineIndex::from_file(normalized_path)).first;
            }
            return it->second;
        }

        void invalidate_line_index(std::string const& normalized_path)
        {
            line_index_cache.erase(normalized_path);
        }

        void invalidate_line_indexes(std::span<ModuleDependency const> dependencies)
        {
            for (auto const& dependency : dependencies) {
                if (dependency.entry_file.empty()) {
                    continue;
                }
                invalidate_line_index(normalized_path_string(dependency.entry_file));
            }
        }

        std::pair<uint32_t, uint32_t> byte_range_for(
            std::string const& normalized_path,
            SourceRange const& range
        ) const
        {
            LineIndex const& index = line_index_for(normalized_path);
            return {
                static_cast<uint32_t>(index.offset_for(range.start)),
                static_cast<uint32_t>(index.offset_for(range.end))
            };
        }

        LiveSourceSpan to_live_span(SnapshotNodeSpan const& span) const
        {
            LineIndex const& index = line_index_for(span.file_path);
            return LiveSourceSpan {
                .file_path = span.file_path,
                .range = SourceRange {
                    .start = index.position_for(span.begin),
                    .end = index.position_for(span.end),
                },
            };
        }

        LogicalNodeInfo to_logical_node(LogicalSnapshotNode const& node) const
        {
            LogicalNodeInfo live;
            live.id = node.id;
            live.kind = node.kind;
            live.sample_inputs = node.sample_inputs;
            live.sample_outputs = node.sample_outputs;
            live.event_inputs = node.event_inputs;
            live.event_outputs = node.event_outputs;
            live.member_count = node.backing_node_ids.size();
            live.source_spans.reserve(node.source_spans.size());
            for (auto const& span : node.source_spans) {
                live.source_spans.push_back(to_live_span(span));
            }
            return live;
        }

        void run_runtime()
        {
            try {
                config = load_runtime_project_config(workspace_root);

                auto audio_device = audio_device_factory
                    ? audio_device_factory()
                    : make_default_audio_device();
                if (!audio_device) {
                    throw std::runtime_error("no production audio backend is currently configured");
                }

#if IV_ENABLE_JUCE_VST
                warmup_juce_vst_scan_cache();
#endif

                auto search_roots = parse_search_path_env();
                search_roots.insert(search_roots.end(), extra_search_roots.begin(), extra_search_roots.end());

                ModuleLoader loader(
                    discovery_start,
                    std::move(search_roots),
                    config->toolchain,
                    [this](std::string const& message) {
                        emit_log("info", message);
                    }
                );
                auto watcher = make_dependency_watcher();

                RenderConfig const render_config = audio_device->config();
                Sample device_sample_period = sample_period(render_config);

                auto loaded_graph = loader.load_root(
                    config->module_root,
                    module_render_config(render_config),
                    &device_sample_period
                );
                watcher->update(loaded_graph.dependencies);
                invalidate_line_indexes(loaded_graph.dependencies);

                std::vector<std::string> created_node_ids;
                {
                    std::scoped_lock lock(mutex);
                    snapshot = build_graph_snapshot(
                        loaded_graph.root,
                        loaded_graph.introspection,
                        config->module_root,
                        loaded_graph.module_id,
                        1
                    );
                    created_node_ids = assign_logical_node_ids(*snapshot, {});
                    initialized = true;
                }
                initialized_cv.notify_all();

                DeviceOrchestrator output_devices(make_audio_device_provider(std::move(*audio_device)));
                auto executor_storage = make_executor(render_config, output_devices, std::move(loaded_graph));
                {
                    std::scoped_lock lock(mutex);
                    executor_state = &executor_storage;
                    if (shutdown_requested) {
                        executor_state->request_shutdown();
                    }
                }

                ReloadWorker reload_worker(
                    *watcher,
                    config->module_root,
                    [&]() {
                        auto reload = loader.load_root(
                            config->module_root,
                            module_render_config(render_config),
                            &device_sample_period
                        );
                        invalidate_line_indexes(reload.dependencies);
                        uint64_t next_epoch = 1;
                        std::vector<std::string> deleted_node_ids;
                        std::vector<std::string> created_node_ids;
                        {
                            std::scoped_lock lock(mutex);
                            next_epoch = snapshot.has_value() ? snapshot->execution_epoch + 1 : 1;
                            std::unordered_map<std::string, std::string> previous_ids_by_stable_key;
                            std::unordered_set<std::string> previous_logical_ids;
                            if (snapshot.has_value()) {
                                deleted_node_ids.reserve(snapshot->logical_nodes.size());
                                for (auto const& node : snapshot->logical_nodes) {
                                    deleted_node_ids.push_back(node.id);
                                    previous_logical_ids.insert(node.id);
                                    if (!node.stable_identity_key.empty()) {
                                        previous_ids_by_stable_key.emplace(node.stable_identity_key, node.id);
                                    }
                                }
                            }
                            snapshot = build_graph_snapshot(
                                reload.root,
                                reload.introspection,
                                config->module_root,
                                reload.module_id,
                                next_epoch
                            );
                            created_node_ids = assign_logical_node_ids(*snapshot, previous_ids_by_stable_key);
                            for (auto const& node : snapshot->logical_nodes) {
                                previous_logical_ids.erase(node.id);
                            }
                            deleted_node_ids.assign(previous_logical_ids.begin(), previous_logical_ids.end());
                        }
                        emit_event(RuntimeProjectEvent {
                            .kind = RuntimeProjectEventKind::build_finished,
                            .level = "info",
                            .message = "rebuild complete " + config->module_root.string(),
                            .module_root = config->module_root,
                            .execution_epoch = next_epoch,
                            .created_node_ids = std::move(created_node_ids),
                            .deleted_node_ids = std::move(deleted_node_ids),
                        });
                        return reload;
                    },
                    [this]() {
                        emit_event(RuntimeProjectEvent {
                            .kind = RuntimeProjectEventKind::build_started,
                            .level = "info",
                            .message = "rebuilding " + config->module_root.string(),
                            .module_root = config->module_root,
                        });
                    },
                    []() {},
                    [this](std::exception_ptr exception) {
                        emit_event(RuntimeProjectEvent {
                            .kind = RuntimeProjectEventKind::build_failed,
                            .level = "error",
                            .message = describe_exception(exception),
                            .module_root = config->module_root,
                        });
                    }
                );
                reload_worker.start();

                executor_storage.execute([&]() -> std::optional<ModuleLoader::LoadedGraph> {
                    if (auto exception = reload_worker.take_exception()) {
                        log_reload_exception("runtime project reload failed", exception);
                    }

                    std::vector<ModuleDependency> dependencies;
                    auto reload = reload_worker.take_completed_reload(&dependencies);
                    if (reload) {
                        watcher->update(std::move(dependencies));
                    }
                    return reload;
                });

                reload_worker.request_shutdown();
            } catch (...) {
                {
                    std::scoped_lock lock(mutex);
                    pending_exception = std::current_exception();
                    initialized = true;
                }
                initialized_cv.notify_all();
            }
        }
    };

    RuntimeProjectService::RuntimeProjectService(
        std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots,
        AudioDeviceFactory audio_device_factory,
        RuntimeProjectEventSink event_sink
    ) :
        _impl(std::make_unique<Impl>(
            std::move(workspace_root),
            std::move(discovery_start),
            std::move(extra_search_roots),
            std::move(audio_device_factory),
            std::move(event_sink)
        ))
    {}

    RuntimeProjectService::~RuntimeProjectService()
    {
        request_shutdown();
    }

    RuntimeProjectService::RuntimeProjectService(RuntimeProjectService&&) noexcept = default;
    RuntimeProjectService& RuntimeProjectService::operator=(RuntimeProjectService&&) noexcept = default;

    RuntimeProjectInitializeResult RuntimeProjectService::initialize()
    {
        if (!_impl->runtime_thread.has_value()) {
            _impl->runtime_thread.emplace([this](std::stop_token) {
                _impl->run_runtime();
            });
        }

        std::unique_lock lock(_impl->mutex);
        _impl->initialized_cv.wait(lock, [&] {
            return _impl->initialized || _impl->pending_exception != nullptr;
        });
        _impl->rethrow_if_failed();

        if (!_impl->snapshot.has_value()) {
            throw std::runtime_error("runtime project service failed to produce an initial graph snapshot");
        }

        return RuntimeProjectInitializeResult {
            .module_root = _impl->snapshot->module_root,
            .module_id = _impl->snapshot->module_id,
            .execution_epoch = _impl->snapshot->execution_epoch,
        };
    }

    RuntimeProjectQueryResult RuntimeProjectService::query_by_spans(
        std::filesystem::path const& file_path,
        std::vector<SourceRange> const& ranges,
        SourceRangeMatchMode match_mode
    ) const
    {
        std::scoped_lock lock(_impl->mutex);
        _impl->rethrow_if_failed();
        if (!_impl->snapshot.has_value()) {
            throw std::runtime_error("runtime project service is not initialized");
        }

        std::string const normalized_file_path = normalized_path_string(file_path);
        std::vector<std::pair<uint32_t, uint32_t>> requested_ranges;
        requested_ranges.reserve(ranges.size());
        for (auto const& range : ranges) {
            requested_ranges.push_back(_impl->byte_range_for(normalized_file_path, range));
        }

        RuntimeProjectQueryResult result;
        result.execution_epoch = _impl->snapshot->execution_epoch;

        auto span_touches_range = [](SnapshotNodeSpan const& span, std::pair<uint32_t, uint32_t> const& requested_range) {
            auto const [begin, end] = requested_range;
            if (begin == end) {
                return span.begin <= begin && begin <= span.end;
            }
            return span.begin <= end && begin <= span.end;
        };

        struct RankedLogicalNode {
            size_t logical_index = 0;
            uint32_t best_span_size = std::numeric_limits<uint32_t>::max();
            uint32_t best_distance = std::numeric_limits<uint32_t>::max();
            uint32_t best_begin = std::numeric_limits<uint32_t>::max();
            uint32_t best_end = std::numeric_limits<uint32_t>::max();
        };

        auto span_distance_to_range = [](SnapshotNodeSpan const& span, std::pair<uint32_t, uint32_t> const& requested_range) {
            auto const [begin, end] = requested_range;
            if (span.begin <= end && begin <= span.end) {
                return 0u;
            }
            if (span.end < begin) {
                return begin - span.end;
            }
            return span.begin - end;
        };

        std::vector<RankedLogicalNode> ranked_nodes;
        ranked_nodes.reserve(_impl->snapshot->logical_nodes.size());
        for (size_t logical_index = 0; logical_index < _impl->snapshot->logical_nodes.size(); ++logical_index) {
            auto const& node = _impl->snapshot->logical_nodes[logical_index];
            bool matches = requested_ranges.empty();
            RankedLogicalNode ranked { .logical_index = logical_index };
            if (!requested_ranges.empty()) {
                auto const node_matches_range = [&](std::pair<uint32_t, uint32_t> const& requested_range) {
                    bool any = false;
                    for (auto const& span : node.source_spans) {
                        if (span.file_path != normalized_file_path || !span_touches_range(span, requested_range)) {
                            continue;
                        }
                        any = true;
                        auto const span_size = span.end >= span.begin ? span.end - span.begin : 0u;
                        auto const distance = span_distance_to_range(span, requested_range);
                        ranked.best_span_size = std::min(ranked.best_span_size, span_size);
                        ranked.best_distance = std::min(ranked.best_distance, distance);
                        ranked.best_begin = std::min(ranked.best_begin, span.begin);
                        ranked.best_end = std::min(ranked.best_end, span.end);
                    }
                    return any;
                };
                if (match_mode == SourceRangeMatchMode::union_) {
                    matches = std::ranges::any_of(requested_ranges, node_matches_range);
                } else {
                    matches = std::ranges::all_of(requested_ranges, node_matches_range);
                }
            } else if (!node.source_spans.empty()) {
                ranked.best_span_size = node.source_spans.front().end >= node.source_spans.front().begin
                    ? node.source_spans.front().end - node.source_spans.front().begin
                    : 0u;
                ranked.best_distance = 0u;
                ranked.best_begin = node.source_spans.front().begin;
                ranked.best_end = node.source_spans.front().end;
            }
            if (!matches) {
                continue;
            }
            ranked_nodes.push_back(ranked);
        }

        std::sort(ranked_nodes.begin(), ranked_nodes.end(), [&](auto const& a, auto const& b) {
            if (a.best_span_size != b.best_span_size) {
                return a.best_span_size < b.best_span_size;
            }
            if (a.best_distance != b.best_distance) {
                return a.best_distance < b.best_distance;
            }
            if (a.best_begin != b.best_begin) {
                return a.best_begin < b.best_begin;
            }
            if (a.best_end != b.best_end) {
                return a.best_end < b.best_end;
            }
            auto const& a_node = _impl->snapshot->logical_nodes[a.logical_index];
            auto const& b_node = _impl->snapshot->logical_nodes[b.logical_index];
            if (a_node.kind != b_node.kind) {
                return a_node.kind < b_node.kind;
            }
            return a_node.id < b_node.id;
        });

        std::unordered_set<std::string> emitted;
        for (auto const& ranked : ranked_nodes) {
            auto const& node = _impl->snapshot->logical_nodes[ranked.logical_index];
            if (emitted.contains(node.id)) {
                continue;
            }
            emitted.insert(node.id);
            result.nodes.push_back(_impl->to_logical_node(node));
        }

        return result;
    }

    RuntimeProjectRegionQueryResult RuntimeProjectService::query_active_regions(
        std::filesystem::path const& file_path
    ) const
    {
        std::scoped_lock lock(_impl->mutex);
        _impl->rethrow_if_failed();
        if (!_impl->snapshot.has_value()) {
            throw std::runtime_error("runtime project service is not initialized");
        }

        std::string const normalized_file_path = normalized_path_string(file_path);
        RuntimeProjectRegionQueryResult result;
        result.execution_epoch = _impl->snapshot->execution_epoch;

        std::unordered_set<std::string> emitted_spans;
        for (size_t logical_index = 0; logical_index < _impl->snapshot->logical_nodes.size(); ++logical_index) {
            auto const& node = _impl->snapshot->logical_nodes[logical_index];
            for (auto const& span : node.source_spans) {
                if (span.file_path != normalized_file_path) {
                    continue;
                }
                auto live_span = _impl->to_live_span(span);
                auto const key =
                    live_span.file_path + ":" +
                    std::to_string(live_span.range.start.line) + ":" +
                    std::to_string(live_span.range.start.column) + ":" +
                    std::to_string(live_span.range.end.line) + ":" +
                    std::to_string(live_span.range.end.column);
                if (emitted_spans.insert(key).second) {
                    result.source_spans.push_back(std::move(live_span));
                }
            }
        }

        return result;
    }

    LogicalNodeInfo RuntimeProjectService::get_logical_node(uint64_t execution_epoch, std::string const& node_id) const
    {
        std::scoped_lock lock(_impl->mutex);
        _impl->rethrow_if_failed();
        if (!_impl->snapshot.has_value()) {
            throw std::runtime_error("runtime project service is not initialized");
        }
        if (_impl->snapshot->execution_epoch != execution_epoch) {
            throw std::runtime_error("stale execution epoch for node query");
        }

        auto const it = _impl->snapshot->logical_node_index_by_id.find(node_id);
        if (it == _impl->snapshot->logical_node_index_by_id.end()) {
            throw std::runtime_error("unknown node id: " + node_id);
        }
        return _impl->to_logical_node(_impl->snapshot->logical_nodes[it->second]);
    }

    std::vector<LogicalNodeInfo> RuntimeProjectService::get_logical_nodes(
        uint64_t execution_epoch,
        std::vector<std::string> const& node_ids
    ) const
    {
        std::scoped_lock lock(_impl->mutex);
        _impl->rethrow_if_failed();
        if (!_impl->snapshot.has_value()) {
            throw std::runtime_error("runtime project service is not initialized");
        }
        if (_impl->snapshot->execution_epoch != execution_epoch) {
            throw std::runtime_error("stale execution epoch for node query");
        }

        std::vector<LogicalNodeInfo> nodes;
        nodes.reserve(node_ids.size());
        for (auto const& node_id : node_ids) {
            auto const it = _impl->snapshot->logical_node_index_by_id.find(node_id);
            if (it == _impl->snapshot->logical_node_index_by_id.end()) {
                throw std::runtime_error("unknown node id: " + node_id);
            }
            nodes.push_back(_impl->to_logical_node(_impl->snapshot->logical_nodes[it->second]));
        }
        return nodes;
    }

    void RuntimeProjectService::request_shutdown()
    {
        if (!_impl) {
            return;
        }

        std::scoped_lock lock(_impl->mutex);
        _impl->shutdown_requested = true;
        if (_impl->executor_state) {
            _impl->executor_state->request_shutdown();
        }
    }
}
