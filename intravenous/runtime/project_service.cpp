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
        };

        struct SnapshotNode {
            std::string id;
            std::string kind;
            std::vector<SnapshotNodeSpan> source_spans;
            std::vector<LivePortInfo> sample_inputs;
            std::vector<LivePortInfo> sample_outputs;
            std::vector<LivePortInfo> event_inputs;
            std::vector<LivePortInfo> event_outputs;
        };

        struct GraphSnapshot {
            uint64_t execution_epoch = 0;
            std::filesystem::path module_root;
            std::string module_id;
            std::vector<SnapshotNode> nodes;
            std::unordered_map<std::string, size_t> node_index_by_id;
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
            std::filesystem::path const& module_root,
            std::string const& module_id,
            uint64_t execution_epoch
        )
        {
            Graph const& graph = require_root_graph(root);

            std::unordered_set<GraphEdge> const& sample_edges = graph._edges;
            std::unordered_set<GraphEventEdge> const& event_edges = graph._event_edges;

            auto sample_input_connected = [&](size_t node, size_t port) {
                return std::ranges::any_of(sample_edges, [&](GraphEdge const& edge) {
                    return edge.target.node == node && edge.target.port == port;
                });
            };
            auto sample_output_connected = [&](size_t node, size_t port) {
                return std::ranges::any_of(sample_edges, [&](GraphEdge const& edge) {
                    return edge.source.node == node && edge.source.port == port;
                });
            };
            auto event_input_connected = [&](size_t node, size_t port) {
                return std::ranges::any_of(event_edges, [&](GraphEventEdge const& edge) {
                    return edge.target.node == node && edge.target.port == port;
                });
            };
            auto event_output_connected = [&](size_t node, size_t port) {
                return std::ranges::any_of(event_edges, [&](GraphEventEdge const& edge) {
                    return edge.source.node == node && edge.source.port == port;
                });
            };

            std::vector<std::optional<SnapshotNode>> by_global_index(graph._node_ids.size());
            for (GraphSccWrapper const& scc : graph._scc_wrappers) {
                for (size_t local_i = 0; local_i < scc._nodes.size(); ++local_i) {
                    size_t const global_i = scc._global_node_indices[local_i];
                    GraphNodeWrapper const& node = scc._nodes[local_i];

                    SnapshotNode snapshot_node;
                    snapshot_node.id = graph._node_ids[global_i];
                    snapshot_node.kind = demangle_type_name(node._node.type_name());

                    if (global_i < graph._node_source_spans.size()) {
                        for (SourceSpan const& span : graph._node_source_spans[global_i]) {
                            snapshot_node.source_spans.push_back(SnapshotNodeSpan {
                                .file_path = normalized_path_string(span.file_path),
                                .begin = span.begin,
                                .end = span.end,
                            });
                        }
                    }

                    auto const inputs = node.inputs();
                    snapshot_node.sample_inputs.reserve(inputs.size());
                    for (size_t input_i = 0; input_i < inputs.size(); ++input_i) {
                        snapshot_node.sample_inputs.push_back(LivePortInfo {
                            .name = inputs[input_i].name,
                            .type = "sample",
                            .connected = sample_input_connected(global_i, input_i),
                        });
                    }

                    auto const outputs = node.outputs();
                    snapshot_node.sample_outputs.reserve(outputs.size());
                    for (size_t output_i = 0; output_i < outputs.size(); ++output_i) {
                        snapshot_node.sample_outputs.push_back(LivePortInfo {
                            .name = outputs[output_i].name,
                            .type = "sample",
                            .connected = sample_output_connected(global_i, output_i),
                        });
                    }

                    auto const event_inputs = node.event_inputs();
                    snapshot_node.event_inputs.reserve(event_inputs.size());
                    for (size_t input_i = 0; input_i < event_inputs.size(); ++input_i) {
                        snapshot_node.event_inputs.push_back(LivePortInfo {
                            .name = event_inputs[input_i].name,
                            .type = event_type_name(event_inputs[input_i].type),
                            .connected = event_input_connected(global_i, input_i),
                        });
                    }

                    auto const event_outputs = node.event_outputs();
                    snapshot_node.event_outputs.reserve(event_outputs.size());
                    for (size_t output_i = 0; output_i < event_outputs.size(); ++output_i) {
                        snapshot_node.event_outputs.push_back(LivePortInfo {
                            .name = event_outputs[output_i].name,
                            .type = event_type_name(event_outputs[output_i].type),
                            .connected = event_output_connected(global_i, output_i),
                        });
                    }

                    by_global_index[global_i] = std::move(snapshot_node);
                }
            }

            GraphSnapshot snapshot;
            snapshot.execution_epoch = execution_epoch;
            snapshot.module_root = normalize_path(module_root);
            snapshot.module_id = module_id;
            snapshot.nodes.reserve(by_global_index.size());
            for (auto& maybe_node : by_global_index) {
                if (!maybe_node.has_value()) {
                    continue;
                }
                snapshot.node_index_by_id.emplace(maybe_node->id, snapshot.nodes.size());
                snapshot.nodes.push_back(std::move(*maybe_node));
            }
            return snapshot;
        }
    }

    class RuntimeProjectService::Impl {
    public:
        std::filesystem::path workspace_root;
        std::filesystem::path discovery_start;
        std::vector<std::filesystem::path> extra_search_roots;
        AudioDeviceFactory audio_device_factory;

        mutable std::mutex mutex;
        mutable std::unordered_map<std::string, LineIndex> line_index_cache;
        std::condition_variable initialized_cv;

        std::optional<RuntimeProjectConfig> config;
        std::optional<GraphSnapshot> snapshot;
        std::exception_ptr pending_exception;
        bool initialized = false;
        bool shutdown_requested = false;

        std::optional<std::jthread> runtime_thread;
        NodeExecutor* executor_state = nullptr;

        explicit Impl(
            std::filesystem::path workspace_root_,
            std::filesystem::path discovery_start_,
            std::vector<std::filesystem::path> extra_search_roots_,
            AudioDeviceFactory audio_device_factory_
        ) :
            workspace_root(normalize_path(workspace_root_)),
            discovery_start(std::move(discovery_start_)),
            extra_search_roots(std::move(extra_search_roots_)),
            audio_device_factory(std::move(audio_device_factory_))
        {}

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

        LiveNodeInfo to_live_node(SnapshotNode const& node) const
        {
            LiveNodeInfo live;
            live.id = node.id;
            live.kind = node.kind;
            live.sample_inputs = node.sample_inputs;
            live.sample_outputs = node.sample_outputs;
            live.event_inputs = node.event_inputs;
            live.event_outputs = node.event_outputs;
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

                ModuleLoader loader(discovery_start, std::move(search_roots), config->toolchain);
                auto watcher = make_dependency_watcher();

                RenderConfig const render_config = audio_device->config();
                Sample device_sample_period = sample_period(render_config);

                auto loaded_graph = loader.load_root(
                    config->module_root,
                    module_render_config(render_config),
                    &device_sample_period
                );
                watcher->update(loaded_graph.dependencies);

                {
                    std::scoped_lock lock(mutex);
                    snapshot = build_graph_snapshot(loaded_graph.root, config->module_root, loaded_graph.module_id, 1);
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
                        {
                            std::scoped_lock lock(mutex);
                            uint64_t const next_epoch = snapshot.has_value() ? snapshot->execution_epoch + 1 : 1;
                            snapshot = build_graph_snapshot(reload.root, config->module_root, reload.module_id, next_epoch);
                        }
                        return reload;
                    }
                );
                reload_worker.start();

                executor_storage.execute([&]() -> std::optional<ModuleLoader::LoadedGraph> {
                    if (auto exception = reload_worker.take_exception()) {
                        std::rethrow_exception(exception);
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
        AudioDeviceFactory audio_device_factory
    ) :
        _impl(std::make_unique<Impl>(
            std::move(workspace_root),
            std::move(discovery_start),
            std::move(extra_search_roots),
            std::move(audio_device_factory)
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
        std::vector<SourceRange> const& ranges
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

        std::unordered_set<std::string> emitted;
        for (SnapshotNode const& node : _impl->snapshot->nodes) {
            bool matches = false;
            for (auto const& span : node.source_spans) {
                if (span.file_path != normalized_file_path) {
                    continue;
                }
                for (auto const& [begin, end] : requested_ranges) {
                    if (span.begin < end && begin < span.end) {
                        matches = true;
                        break;
                    }
                }
                if (matches) {
                    break;
                }
            }
            if (!matches || emitted.contains(node.id)) {
                continue;
            }
            emitted.insert(node.id);
            result.nodes.push_back(_impl->to_live_node(node));
        }

        return result;
    }

    LiveNodeInfo RuntimeProjectService::get_node(uint64_t execution_epoch, std::string const& node_id) const
    {
        std::scoped_lock lock(_impl->mutex);
        _impl->rethrow_if_failed();
        if (!_impl->snapshot.has_value()) {
            throw std::runtime_error("runtime project service is not initialized");
        }
        if (_impl->snapshot->execution_epoch != execution_epoch) {
            throw std::runtime_error("stale execution epoch for node query");
        }

        auto const it = _impl->snapshot->node_index_by_id.find(node_id);
        if (it == _impl->snapshot->node_index_by_id.end()) {
            throw std::runtime_error("unknown node id: " + node_id);
        }
        return _impl->to_live_node(_impl->snapshot->nodes[it->second]);
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
