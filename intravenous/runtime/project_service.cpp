#include "runtime/project_service.h"

#include "runtime/config.h"
#include "compat.h"
#include "devices/miniaudio_device.h"
#include "juce/vst_runtime.h"
#include "module/loader.h"
#include "module/search_paths.h"
#include "module/watcher.h"
#include "node/executor.h"
#include "orchestrator/orchestrator_builder.h"
#include "runtime/handlers.h"
#include "runtime/reload_worker.h"
#include "runtime/timeline.h"

#include <algorithm>
#include <array>
#include <condition_variable>
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

        struct GraphSnapshot {
            uint64_t execution_epoch = 0;
            std::filesystem::path module_root;
            std::string module_id;
            std::vector<IntrospectionLogicalNode> logical_nodes;
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

        void sort_and_deduplicate_spans(std::vector<SourceSpan>& spans)
        {
            std::sort(spans.begin(), spans.end(), [](auto const& a, auto const& b) {
                return std::tie(a.file_path, a.begin, a.end) < std::tie(b.file_path, b.begin, b.end);
            });
            spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
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

        GraphSnapshot build_graph_snapshot(
            GraphIntrospectionMetadata const& introspection,
            std::filesystem::path const& module_root,
            std::string const& module_id,
            uint64_t execution_epoch
        )
        {
            GraphSnapshot snapshot;
            snapshot.execution_epoch = execution_epoch;
            snapshot.module_root = normalize_path(module_root);
            snapshot.module_id = module_id;
            snapshot.logical_nodes = introspection.logical_nodes;
            for (auto& logical_node : snapshot.logical_nodes) {
                for (auto& span : logical_node.source_spans) {
                    if (!span.file_path.empty()) {
                        span.file_path = normalized_path_string(span.file_path);
                    }
                }
                sort_and_deduplicate_spans(logical_node.source_spans);
            }
            for (size_t i = 0; i < snapshot.logical_nodes.size(); ++i) {
                snapshot.logical_node_index_by_id.emplace(snapshot.logical_nodes[i].id, i);
            }

            return snapshot;
        }
    }

    class RuntimeProjectService::Impl {
    public:
        Timeline& timeline;
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

        std::optional<std::jthread> runtime_thread;
        NodeExecutor* executor_state = nullptr;

        explicit Impl(
            Timeline& timeline_,
            std::filesystem::path workspace_root_,
            std::filesystem::path discovery_start_,
            std::vector<std::filesystem::path> extra_search_roots_,
            AudioDeviceFactory audio_device_factory_,
            RuntimeProjectEventSink event_sink_
        ) :
            timeline(timeline_),
            workspace_root(normalize_path(workspace_root_)),
            discovery_start(std::move(discovery_start_)),
            extra_search_roots(std::move(extra_search_roots_)),
            audio_device_factory(std::move(audio_device_factory_)),
            event_sink(std::move(event_sink_))
        {}

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

        LiveSourceSpan to_live_span(SourceSpan const& span) const
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

        LogicalNodeInfo to_logical_node(IntrospectionLogicalNode const& node) const
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
                    this->timeline,
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
                watcher.update(loaded_graph.dependencies);
                invalidate_line_indexes(loaded_graph.dependencies);

                std::vector<std::string> created_node_ids;
                {
                    std::scoped_lock lock(mutex);
                    snapshot = build_graph_snapshot(
                        loaded_graph.introspection,
                        config->module_root,
                        loaded_graph.module_id,
                        1
                    );
                    created_node_ids.reserve(snapshot->logical_nodes.size());
                    for (auto const& node : snapshot->logical_nodes) {
                        created_node_ids.push_back(node.id);
                    }
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
                    watcher,
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
                            std::unordered_set<std::string> previous_logical_ids;
                            if (snapshot.has_value()) {
                                deleted_node_ids.reserve(snapshot->logical_nodes.size());
                                for (auto const& node : snapshot->logical_nodes) {
                                    deleted_node_ids.push_back(node.id);
                                    previous_logical_ids.insert(node.id);
                                }
                            }
                            snapshot = build_graph_snapshot(
                                reload.introspection,
                                config->module_root,
                                reload.module_id,
                                next_epoch
                            );
                            for (auto const& node : snapshot->logical_nodes) {
                                if (!previous_logical_ids.erase(node.id)) {
                                    created_node_ids.push_back(node.id);
                                }
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
                    if (!reload_worker.has_pending_reload()) {
                        if (reload_worker.has_pending_exception()) {
                            if (auto exception = reload_worker.take_exception()) {
                                log_reload_exception("runtime project reload failed", exception);
                            }
                        }
                        return std::nullopt;
                    }

                    if (auto exception = reload_worker.take_exception()) {
                        log_reload_exception("runtime project reload failed", exception);
                    }

                    std::vector<ModuleDependency> dependencies;
                    auto reload = reload_worker.take_completed_reload(&dependencies);
                    if (reload) {
                        watcher.update(std::move(dependencies));
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
        Timeline& timeline,
        std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots,
        AudioDeviceFactory audio_device_factory,
        RuntimeProjectEventSink event_sink
    ) :
        _impl(std::make_unique<Impl>(
            timeline,
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

        auto span_touches_range = [](SourceSpan const& span, std::pair<uint32_t, uint32_t> const& requested_range) {
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

        auto span_distance_to_range = [](SourceSpan const& span, std::pair<uint32_t, uint32_t> const& requested_range) {
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
