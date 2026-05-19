#include "runtime/project_service.h"

#include "compat.h"
#include "devices/miniaudio_device.h"
#include "filesystem_paths.h"
#include "juce/vst_runtime.h"
#include "module/loader.h"
#include "module/search_paths.h"
#include "module/watcher.h"
#include "node/executor.h"
#include "orchestrator/orchestrator_builder.h"
#include "runtime/config.h"
#include "runtime/graph_input_lane_controller.h"
#include "runtime/handlers.h"
#include "runtime/reload_worker.h"
#include "runtime/socket_rpc_server.h"
#include "runtime/timeline.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <concepts>
#include <fstream>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

// there is SO MUCH SHIT in this file
// this file should do ONLY ROUTING. read again: ONLY ROUTING
// everything else belongs to separated, orthogonal modules/classes
// you're mixing dozens of responsibilities. not just 2 or 3! literal DOZENS!!!
// WTF
namespace iv {
SourceTextLineMap
SourceTextLineMap::from_file(std::filesystem::path const &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open source file: " + path.string());
  }

  SourceTextLineMap map;
  map.text.assign(std::istreambuf_iterator<char>(in),
                  std::istreambuf_iterator<char>());
  map.line_offsets.push_back(0);
  for (size_t i = 0; i < map.text.size(); ++i) {
    if (map.text[i] == '\n') {
      map.line_offsets.push_back(i + 1);
    }
  }
  return map;
}

size_t SourceTextLineMap::offset_for(SourcePosition position) const {
  if (line_offsets.empty()) {
    return 0;
  }

  size_t const line_index =
      position.line <= 1
          ? 0
          : std::min<size_t>(position.line - 1, line_offsets.size() - 1);
  size_t const line_start = line_offsets[line_index];
  size_t const next_line_start = line_index + 1 < line_offsets.size()
                                     ? line_offsets[line_index + 1]
                                     : text.size();
  size_t const requested_column =
      position.column <= 1 ? 0 : position.column - 1;
  return std::min(line_start + requested_column, next_line_start);
}

SourcePosition SourceTextLineMap::position_for(size_t offset) const {
  offset = std::min(offset, text.size());
  auto const it =
      std::upper_bound(line_offsets.begin(), line_offsets.end(), offset);
  size_t const line_index =
      it == line_offsets.begin()
          ? 0
          : static_cast<size_t>(std::distance(line_offsets.begin(), it - 1));
  return SourcePosition{
      .line = static_cast<uint32_t>(line_index + 1),
      .column = static_cast<uint32_t>(offset - line_offsets[line_index] + 1),
  };
}

namespace {

ModuleExecutorTarget module_executor_target(RenderConfig const &config) {
  return ModuleExecutorTarget{
      .sample_rate = config.sample_rate,
      .num_channels = config.num_channels,
      .max_block_frames = config.max_block_frames,
  };
}

DeviceOrchestrator make_audio_device_provider(LogicalAudioDevice audio_device) {
  std::vector<OutputDeviceMixer> mixers;
  mixers.emplace_back(std::move(audio_device), [](OrchestratorBuilder &builder,
                                                  OutputDeviceMixer &&mixer) {
    builder.add_audio_mixer(0, std::move(mixer));
  });
  return DeviceOrchestrator(std::move(mixers));
}

ResourceContext make_resource_context(RenderConfig const &render_config) {
  ResourceContext resources{};
#if IV_ENABLE_JUCE_VST
  static JuceVstRuntimeManager juce_vst_runtime_manager;
  static std::unique_ptr<JuceVstRuntimeSupport> juce_vst_runtime_support;
  juce_vst_runtime_support = std::make_unique<JuceVstRuntimeSupport>(
      juce_vst_runtime_manager, static_cast<double>(render_config.sample_rate));
  resources = juce_vst_runtime_support->resources();
#endif
  return resources;
}

NodeExecutor make_executor(RenderConfig const &render_config,
                           DeviceOrchestrator &device_orchestrator,
                           ModuleLoader::LoadedGraph loaded_graph) {
  return NodeExecutor::create(std::move(loaded_graph.root),
                              make_resource_context(render_config),
                              std::move(device_orchestrator).to_builder(),
                              std::move(loaded_graph.module_refs));
}

void sort_and_deduplicate_spans(std::vector<SourceSpan> &spans) {
  std::sort(spans.begin(), spans.end(), [](auto const &a, auto const &b) {
    return std::tie(a.file_path, a.begin, a.end) <
           std::tie(b.file_path, b.begin, b.end);
  });
  spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
}

std::string format_exception_context(std::string_view context,
                                     std::exception_ptr exception) {
  if (!exception) {
    return std::string(context);
  }

  try {
    std::rethrow_exception(exception);
  } catch (std::exception const &e) {
    return std::string(context) + ": " + e.what();
  } catch (...) {
    return std::string(context) + ": unknown exception";
  }
}

std::string describe_exception(std::exception_ptr exception) {
  if (!exception) {
    return "unknown exception";
  }

  try {
    std::rethrow_exception(exception);
  } catch (std::exception const &e) {
    return e.what();
  } catch (...) {
    return "unknown exception";
  }
}

LoadedGraphIntrospectionIndex
build_graph_introspection_index(GraphIntrospectionMetadata const &introspection,
                                std::filesystem::path const &module_root,
                                std::string const &module_id) {
  LoadedGraphIntrospectionIndex graph_index;
  graph_index.module_root = normalize_path(module_root);
  graph_index.module_id = module_id;
  graph_index.logical_nodes = introspection.logical_nodes;
  for (auto &logical_node : graph_index.logical_nodes) {
    for (auto &span : logical_node.source_spans) {
      if (!span.file_path.empty()) {
        span.file_path = normalized_path_string(span.file_path);
      }
    }
    sort_and_deduplicate_spans(logical_node.source_spans);
  }
  for (size_t i = 0; i < graph_index.logical_nodes.size(); ++i) {
    graph_index.logical_node_index_by_id.emplace(
        graph_index.logical_nodes[i].id, i);
  }

  return graph_index;
}

void append_graph_input_port_descriptors(
    std::vector<GraphInputPortDescriptor> &ports,
    std::string const &logical_node_id,
    std::optional<size_t> concrete_member_ordinal, PortKind port_kind,
    std::span<LogicalPortInfo const> logical_ports) {
  ports.reserve(ports.size() + logical_ports.size());
  for (auto const &port : logical_ports) {
    ports.push_back(GraphInputPortDescriptor{
        .logical_node_id = logical_node_id,
        .concrete_member_ordinal = concrete_member_ordinal,
        .port_kind = port_kind,
        .port_ordinal = port.ordinal,
        .port_name = port.name,
        .port_type = port.type,
    });
  }
}

std::vector<GraphInputPortDescriptor> graph_input_port_descriptors_for(
    std::span<IntrospectionLogicalNode const> logical_nodes) {
  std::vector<GraphInputPortDescriptor> ports;
  for (auto const &node : logical_nodes) {
    append_graph_input_port_descriptors(ports, node.id, std::nullopt,
                                    PortKind::sample, node.sample_inputs);
    append_graph_input_port_descriptors(ports, node.id, std::nullopt,
                                    PortKind::event, node.event_inputs);
    for (auto const &member : node.members) {
      append_graph_input_port_descriptors(ports, node.id, member.ordinal,
                                      PortKind::sample, member.sample_inputs);
      append_graph_input_port_descriptors(ports, node.id, member.ordinal,
                                      PortKind::event, member.event_inputs);
    }
  }
  return ports;
}

GraphInputPortDescriptor
sample_graph_input_port_for(IntrospectionLogicalNode const &node,
                              std::optional<size_t> concrete_member_ordinal,
                              size_t input_ordinal) {
  auto const &ports = concrete_member_ordinal.has_value()
                          ? node.members[*concrete_member_ordinal].sample_inputs
                          : node.sample_inputs;
  auto const port_it =
      std::ranges::find_if(ports, [&](LogicalPortInfo const &port) {
        return port.ordinal == input_ordinal;
      });
  if (port_it == ports.end()) {
    throw std::runtime_error("unknown sample input ordinal " +
                             std::to_string(input_ordinal));
  }
  return GraphInputPortDescriptor{
      .logical_node_id = node.id,
      .concrete_member_ordinal = concrete_member_ordinal,
      .port_kind = PortKind::sample,
      .port_ordinal = port_it->ordinal,
      .port_name = port_it->name,
      .port_type = port_it->type,
  };
}

} // namespace

void RuntimeProjectService::emit_notification(
    RuntimeProjectNotification notification) {
  std::visit(
      [&](auto &payload) {
        using Payload = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::same_as<Payload, RuntimeProjectLaneViewNotification>) {
          return;
        } else {
          if (payload.module_root.empty()) {
            if (config.has_value()) {
              payload.module_root = config->module_root;
            } else if (graph_index.has_value()) {
              payload.module_root = graph_index->module_root;
            }
          }
        }
      },
      notification);
  if (server != nullptr) {
    server->send_runtime_project_notification(notification);
  }
}

void RuntimeProjectService::emit_message(std::string level,
                                         std::string message) {
  emit_notification(RuntimeProjectMessageNotification{
      .level = std::move(level),
      .message = std::move(message),
  });
}

void RuntimeProjectService::emit_status(
    std::string code, std::string level, std::string message,
    std::filesystem::path module_root, std::vector<std::string> created_node_ids,
    std::vector<std::string> deleted_node_ids) {
  emit_notification(RuntimeProjectNotification{
      RuntimeProjectStatusNotification{
          .level = std::move(level),
          .code = std::move(code),
          .message = std::move(message),
          .module_root = std::move(module_root),
          .created_node_ids = std::move(created_node_ids),
          .deleted_node_ids = std::move(deleted_node_ids),
      },
  });
}

void RuntimeProjectService::rethrow_if_failed() const {
  if (pending_exception) {
    std::rethrow_exception(pending_exception);
  }
}

SourceTextLineMap const &RuntimeProjectService::source_text_for(
    std::string const &normalized_path) const {
  auto it = source_text_cache.find(normalized_path);
  if (it == source_text_cache.end()) {
    it = source_text_cache
             .emplace(normalized_path,
                      SourceTextLineMap::from_file(normalized_path))
             .first;
  }
  return it->second;
}

void RuntimeProjectService::invalidate_source_text(
    std::string const &normalized_path) {
  source_text_cache.erase(normalized_path);
}

void RuntimeProjectService::invalidate_source_texts(
    std::span<ModuleDependency const> dependencies) {
  for (auto const &dependency : dependencies) {
    if (dependency.entry_file.empty()) {
      continue;
    }
    invalidate_source_text(normalized_path_string(dependency.entry_file));
  }
}

std::pair<uint32_t, uint32_t>
RuntimeProjectService::byte_range_for(std::string const &normalized_path,
                                      SourceRange const &range) const {
  SourceTextLineMap const &index = source_text_for(normalized_path);
  return {static_cast<uint32_t>(index.offset_for(range.start)),
          static_cast<uint32_t>(index.offset_for(range.end))};
}

LiveSourceSpan
RuntimeProjectService::to_live_span(SourceSpan const &span) const {
  SourceTextLineMap const &index = source_text_for(span.file_path);
  return LiveSourceSpan{
      .file_path = span.file_path,
      .range =
          SourceRange{
              .start = index.position_for(span.begin),
              .end = index.position_for(span.end),
          },
  };
}

LogicalNodeInfo RuntimeProjectService::to_logical_node(
    IntrospectionLogicalNode const &node) const {
  LogicalNodeInfo live;
  live.id = node.id;
  live.kind = node.kind;
  live.source_identity = node.source_identity;
  live.type_identity = node.type_identity;
  live.sample_inputs = node.sample_inputs;
  for (auto &port : live.sample_inputs) {
    port.current_value =
        timeline.live_input_value_or(node.id, port.ordinal, port.default_value);
  }
  live.sample_outputs = node.sample_outputs;
  live.event_inputs = node.event_inputs;
  live.event_outputs = node.event_outputs;
  live.member_count = node.backing_node_ids.size();
  live.members.reserve(node.members.size());
  for (auto const &member : node.members) {
    LogicalNodeMemberInfo live_member;
    live_member.ordinal = member.ordinal;
    live_member.backing_node_id = member.backing_node_id;
    live_member.kind = member.kind;
    live_member.type_identity = member.type_identity;
    live_member.sample_inputs = member.sample_inputs;
    for (auto &port : live_member.sample_inputs) {
      port.current_value = timeline.live_input_value_or(
          node.id, member.ordinal, port.ordinal,
          timeline.live_input_value_or(node.id, port.ordinal,
                                       port.default_value));
      port.has_concrete_override = timeline.has_live_input_value_override(
          node.id, member.ordinal, port.ordinal);
    }
    live_member.sample_outputs = member.sample_outputs;
    live_member.event_inputs = member.event_inputs;
    live_member.event_outputs = member.event_outputs;
    live.members.push_back(std::move(live_member));
  }
  live.source_spans.reserve(node.source_spans.size());
  for (auto const &span : node.source_spans) {
    live.source_spans.push_back(to_live_span(span));
  }
  return live;
}

LaneQueryResult RuntimeProjectService::query_lanes_locked(
    LaneQueryFilter const &filter,
    std::optional<size_t> start_index,
    std::optional<size_t> visible_lane_count) const {
  if (!graph_index.has_value()) {
    throw std::runtime_error("runtime project service is not initialized");
  }
  if (filter.kind != "graphInputs") {
    throw std::runtime_error("unsupported lane filter: " + filter.kind);
  }

  LaneQueryResult result;
  auto const controls = timeline.reconcile_graph_input_lane_bindings(
      graph_input_port_descriptors_for(graph_index->logical_nodes));

  std::vector<LaneInfo> lane_infos;
  lane_infos.reserve(controls.logical_sample_knobs.size() +
                     controls.sample_inputs.size() +
                     controls.event_inputs.size());
  for (auto const &control : controls.logical_sample_knobs) {
    lane_infos.push_back(LaneInfo{
        .lane_id = control.knob_lane.value,
        .domain = LaneDomain::realtime,
        .graph_input_port = control.port,
    });
  }
  for (auto const &control : controls.sample_inputs) {
    lane_infos.push_back(LaneInfo{
        .lane_id = control.knob_lane.value,
        .domain = LaneDomain::realtime,
        .graph_input_port = control.port,
    });
  }
  for (auto const &event_input : controls.event_inputs) {
    lane_infos.push_back(LaneInfo{
        .lane_id = event_input.graph_input_lane.value,
        .domain = LaneDomain::realtime,
        .graph_input_port = event_input.port,
    });
  }

  result.total_lane_count = lane_infos.size();
  result.start_index = start_index.value_or(0);
  result.visible_lane_count =
      visible_lane_count.value_or(result.total_lane_count);

  size_t const max_start_index =
      result.total_lane_count == 0
          ? 0
          : (result.visible_lane_count >= result.total_lane_count
                 ? 0
                 : result.total_lane_count - result.visible_lane_count);
  size_t const lane_begin = std::min(result.start_index, max_start_index);
  size_t const remaining_lanes = result.total_lane_count - lane_begin;
  size_t const window_lane_count =
      std::min(result.visible_lane_count, remaining_lanes);
  size_t const lane_end = lane_begin + window_lane_count;
  bool const restrict_connections_to_window =
      start_index.has_value() || visible_lane_count.has_value();
  result.start_index = lane_begin;
  result.lanes.reserve(window_lane_count);

  std::unordered_set<uint64_t> visible_lane_ids;
  visible_lane_ids.reserve(window_lane_count);
  for (size_t lane_index = lane_begin; lane_index < lane_end; ++lane_index) {
    auto const &lane = lane_infos[lane_index];
    visible_lane_ids.insert(lane.lane_id);
    result.lanes.push_back(lane);
  }

  for (auto const &lane : lane_infos) {
    if (!visible_lane_ids.contains(lane.lane_id) &&
        restrict_connections_to_window) {
      continue;
    }
    LaneId const source{lane.lane_id};
    for (auto const &output : timeline.lane_outputs_for(source)) {
      if (restrict_connections_to_window &&
          !visible_lane_ids.contains(source.value) &&
          !visible_lane_ids.contains(output.target.value)) {
        continue;
      }
      result.connections.push_back(LaneConnectionInfo{
          .source_lane_id = source.value,
          .target_lane_id = output.target.value,
          .port_kind = output.input.kind,
          .port_ordinal = output.input.ordinal,
      });
    }
  }
  return result;
}

void RuntimeProjectService::run_runtime() {
  try {
    config = load_runtime_project_config(workspace_root);

    auto audio_device = audio_device_factory ? audio_device_factory()
                                             : make_miniaudio_device({});
    if (!audio_device) {
      throw std::runtime_error(
          "no production audio backend is currently configured");
    }

#if IV_ENABLE_JUCE_VST
    warmup_juce_vst_scan_cache();
#endif

    auto search_roots = parse_search_path_env();
    search_roots.insert(search_roots.end(), extra_search_roots.begin(),
                        extra_search_roots.end());

    ModuleLoader loader(
        this->timeline, discovery_start, std::move(search_roots),
        config->toolchain,
        [this](std::string const &message) { emit_message("info", message); });
    auto watcher = make_dependency_watcher();

    RenderConfig const render_config = audio_device->config();
    Sample device_sample_period = sample_period(render_config);

    auto loaded_graph = loader.load_root(config->module_root,
                                         module_executor_target(render_config),
                                         &device_sample_period);
    watcher.update(loaded_graph.dependencies);
    invalidate_source_texts(loaded_graph.dependencies);

    std::vector<std::string> created_node_ids;
    {
      std::scoped_lock lock(mutex);
      graph_index = build_graph_introspection_index(loaded_graph.introspection,
                                                    config->module_root,
                                                    loaded_graph.module_id);
      timeline.ensure_graph_input_lane_bindings(
          graph_input_port_descriptors_for(graph_index->logical_nodes));
      lane_views.mark_lane_set_changed();
      created_node_ids.reserve(graph_index->logical_nodes.size());
      for (auto const &node : graph_index->logical_nodes) {
        created_node_ids.push_back(node.id);
      }
      initialized = true;
    }
    initialized_cv.notify_all();

    DeviceOrchestrator output_devices(
        make_audio_device_provider(std::move(*audio_device)));
    auto executor_storage =
        make_executor(render_config, output_devices, std::move(loaded_graph));
    {
      std::scoped_lock lock(mutex);
      executor_state = &executor_storage;
      if (shutdown_requested) {
        executor_state->request_shutdown();
      }
    }

    ReloadWorker reload_worker(
        watcher, config->module_root,
        [&]() {
          auto reload = loader.load_root(config->module_root,
                                         module_executor_target(render_config),
                                         &device_sample_period);
          invalidate_source_texts(reload.dependencies);
          std::vector<std::string> deleted_node_ids;
          std::vector<std::string> created_node_ids;
          {
            std::scoped_lock lock(mutex);
            std::unordered_set<std::string> previous_logical_ids;
            if (graph_index.has_value()) {
              deleted_node_ids.reserve(graph_index->logical_nodes.size());
              for (auto const &node : graph_index->logical_nodes) {
                deleted_node_ids.push_back(node.id);
                previous_logical_ids.insert(node.id);
              }
            }
            graph_index = build_graph_introspection_index(
                reload.introspection, config->module_root, reload.module_id);
            timeline.ensure_graph_input_lane_bindings(
                graph_input_port_descriptors_for(graph_index->logical_nodes));
            lane_views.mark_lane_set_changed();
            for (auto const &node : graph_index->logical_nodes) {
              if (!previous_logical_ids.erase(node.id)) {
                created_node_ids.push_back(node.id);
              }
            }
            deleted_node_ids.assign(previous_logical_ids.begin(),
                                    previous_logical_ids.end());
          }
          emit_status("rebuildFinished", "info",
                      "rebuild complete " + config->module_root.string(),
                      config->module_root, std::move(created_node_ids),
                      std::move(deleted_node_ids));
          return reload;
        },
        [this]() {
          emit_status("rebuildStarted", "info",
                      "rebuilding " + config->module_root.string(),
                      config->module_root);
        },
        []() {},
        [this](std::exception_ptr exception) {
          emit_status("rebuildFailed", "error", describe_exception(exception),
                      config->module_root);
        });
    reload_worker.start();

    executor_storage.execute([&]() -> std::optional<ModuleLoader::LoadedGraph> {
      if (!reload_worker.has_pending_reload()) {
        if (reload_worker.has_pending_exception()) {
          if (auto exception = reload_worker.take_exception()) {
            emit_message("error",
                         format_exception_context(
                             "runtime project reload failed", exception));
          }
        }
        return std::nullopt;
      }

      if (auto exception = reload_worker.take_exception()) {
        emit_message("error", format_exception_context(
                                  "runtime project reload failed", exception));
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
    auto exception = std::current_exception();
    emit_status("startupFailed", "error", describe_exception(exception));
    {
      std::scoped_lock lock(mutex);
      pending_exception = exception;
      initialized = true;
    }
    initialized_cv.notify_all();
  }
}

void RuntimeProjectService::configure_lane_views() {
  lane_views.set_query_provider(
      [this](LaneQueryFilter const &filter,
             std::optional<size_t> start_index,
             std::optional<size_t> visible_lane_count) {
        return query_lanes_locked(filter, start_index, visible_lane_count);
      });
  lane_views.set_update_sink([this](LaneViewResult const &update) {
    emit_notification(RuntimeProjectLaneViewNotification{
        .lane_view = update,
    });
  });
}

RuntimeProjectService::RuntimeProjectService(
    Timeline &timeline, std::filesystem::path workspace_root,
    std::filesystem::path discovery_start,
    std::vector<std::filesystem::path> extra_search_roots,
    AudioDeviceFactory audio_device_factory)
    : timeline(timeline), workspace_root(normalize_path(workspace_root)),
      discovery_start(std::move(discovery_start)),
      extra_search_roots(std::move(extra_search_roots)),
      audio_device_factory(std::move(audio_device_factory)) {
  configure_lane_views();
}

RuntimeProjectService::RuntimeProjectService(
    Timeline &timeline, SocketRpcServer &server,
    std::filesystem::path workspace_root, std::filesystem::path discovery_start,
    std::vector<std::filesystem::path> extra_search_roots,
    AudioDeviceFactory audio_device_factory)
    : timeline(timeline), server(&server),
      workspace_root(normalize_path(workspace_root)),
      discovery_start(std::move(discovery_start)),
      extra_search_roots(std::move(extra_search_roots)),
      audio_device_factory(std::move(audio_device_factory)) {
  server.attach_service(*this);
  configure_lane_views();
}

RuntimeProjectService::~RuntimeProjectService() {
  if (server != nullptr) {
    server->detach_service(*this);
  }
  request_shutdown();
}

RuntimeProjectInitializeResult RuntimeProjectService::initialize() {
  if (!runtime_thread.has_value()) {
    runtime_thread.emplace([this](std::stop_token) { run_runtime(); });
  }

  std::unique_lock lock(mutex);
  initialized_cv.wait(
      lock, [&] { return initialized || pending_exception != nullptr; });
  rethrow_if_failed();

  if (!graph_index.has_value()) {
    throw std::runtime_error("runtime project service failed to produce an "
                             "initial graph graph_index");
  }

  return RuntimeProjectInitializeResult{
      .module_root = graph_index->module_root,
      .module_id = graph_index->module_id,
  };
}

RuntimeProjectQueryResult
RuntimeProjectService::query_by_spans(std::filesystem::path const &file_path,
                                      std::vector<SourceRange> const &ranges,
                                      SourceRangeMatchMode match_mode) const {
  std::scoped_lock lock(mutex);
  rethrow_if_failed();
  if (!graph_index.has_value()) {
    throw std::runtime_error("runtime project service is not initialized");
  }

  std::string const normalized_file_path = normalized_path_string(file_path);
  std::vector<std::pair<uint32_t, uint32_t>> requested_ranges;
  requested_ranges.reserve(ranges.size());
  for (auto const &range : ranges) {
    requested_ranges.push_back(byte_range_for(normalized_file_path, range));
  }

  RuntimeProjectQueryResult result;

  auto span_touches_range =
      [](SourceSpan const &span,
         std::pair<uint32_t, uint32_t> const &requested_range) {
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

  auto span_distance_to_range =
      [](SourceSpan const &span,
         std::pair<uint32_t, uint32_t> const &requested_range) {
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
  ranked_nodes.reserve(graph_index->logical_nodes.size());
  for (size_t logical_index = 0;
       logical_index < graph_index->logical_nodes.size(); ++logical_index) {
    auto const &node = graph_index->logical_nodes[logical_index];
    bool matches = requested_ranges.empty();
    RankedLogicalNode ranked{.logical_index = logical_index};
    if (!requested_ranges.empty()) {
      auto const node_matches_range =
          [&](std::pair<uint32_t, uint32_t> const &requested_range) {
            bool any = false;
            for (auto const &span : node.source_spans) {
              if (span.file_path != normalized_file_path ||
                  !span_touches_range(span, requested_range)) {
                continue;
              }
              any = true;
              auto const span_size =
                  span.end >= span.begin ? span.end - span.begin : 0u;
              auto const distance =
                  span_distance_to_range(span, requested_range);
              ranked.best_span_size =
                  std::min(ranked.best_span_size, span_size);
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
      ranked.best_span_size =
          node.source_spans.front().end >= node.source_spans.front().begin
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

  std::sort(ranked_nodes.begin(), ranked_nodes.end(),
            [&](auto const &a, auto const &b) {
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
              auto const &a_node = graph_index->logical_nodes[a.logical_index];
              auto const &b_node = graph_index->logical_nodes[b.logical_index];
              if (a_node.kind != b_node.kind) {
                return a_node.kind < b_node.kind;
              }
              return a_node.id < b_node.id;
            });

  std::unordered_set<std::string> emitted;
  for (auto const &ranked : ranked_nodes) {
    auto const &node = graph_index->logical_nodes[ranked.logical_index];
    if (emitted.contains(node.id)) {
      continue;
    }
    emitted.insert(node.id);
    result.nodes.push_back(to_logical_node(node));
  }

  return result;
}

RuntimeProjectRegionQueryResult RuntimeProjectService::query_active_regions(
    std::filesystem::path const &file_path) const {
  std::scoped_lock lock(mutex);
  rethrow_if_failed();
  if (!graph_index.has_value()) {
    throw std::runtime_error("runtime project service is not initialized");
  }

  std::string const normalized_file_path = normalized_path_string(file_path);
  RuntimeProjectRegionQueryResult result;

  std::unordered_set<std::string> emitted_spans;
  for (size_t logical_index = 0;
       logical_index < graph_index->logical_nodes.size(); ++logical_index) {
    auto const &node = graph_index->logical_nodes[logical_index];
    for (auto const &span : node.source_spans) {
      if (span.file_path != normalized_file_path) {
        continue;
      }
      auto live_span = to_live_span(span);
      auto const key = live_span.file_path + ":" +
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

LogicalNodeInfo
RuntimeProjectService::get_logical_node(std::string const &node_id) const {
  std::scoped_lock lock(mutex);
  rethrow_if_failed();
  if (!graph_index.has_value()) {
    throw std::runtime_error("runtime project service is not initialized");
  }

  auto const it = graph_index->logical_node_index_by_id.find(node_id);
  if (it == graph_index->logical_node_index_by_id.end()) {
    throw std::runtime_error("unknown node id: " + node_id);
  }
  return to_logical_node(graph_index->logical_nodes[it->second]);
}

std::vector<LogicalNodeInfo> RuntimeProjectService::get_logical_nodes(
    std::vector<std::string> const &node_ids) const {
  std::scoped_lock lock(mutex);
  rethrow_if_failed();
  if (!graph_index.has_value()) {
    throw std::runtime_error("runtime project service is not initialized");
  }

  std::vector<LogicalNodeInfo> nodes;
  nodes.reserve(node_ids.size());
  for (auto const &node_id : node_ids) {
    auto const it = graph_index->logical_node_index_by_id.find(node_id);
    if (it == graph_index->logical_node_index_by_id.end()) {
      throw std::runtime_error("unknown node id: " + node_id);
    }
    nodes.push_back(to_logical_node(graph_index->logical_nodes[it->second]));
  }
  return nodes;
}

LaneViewResult RuntimeProjectService::open_lane_view(LaneViewRequest request) {
  std::scoped_lock lock(mutex);
  rethrow_if_failed();
  return lane_views.open_view(std::move(request));
}

LaneViewResult
RuntimeProjectService::update_lane_view(LaneViewRequest request) {
  std::scoped_lock lock(mutex);
  rethrow_if_failed();
  return lane_views.update_view(std::move(request));
}

void RuntimeProjectService::close_lane_view(std::string const &view_id) {
  std::scoped_lock lock(mutex);
  lane_views.close_view(view_id);
}

void RuntimeProjectService::set_sample_input_value(
    std::string const &node_id, size_t input_ordinal, Sample value,
    std::optional<size_t> member_ordinal) {
  std::scoped_lock lock(mutex);
  rethrow_if_failed();
  if (!graph_index.has_value()) {
    throw std::runtime_error("runtime project service is not initialized");
  }
  auto const it = graph_index->logical_node_index_by_id.find(node_id);
  if (it == graph_index->logical_node_index_by_id.end()) {
    throw std::runtime_error("unknown node id: " + node_id);
  }
  if (member_ordinal.has_value()) {
    auto const &node = graph_index->logical_nodes[it->second];
    if (*member_ordinal >= node.members.size()) {
      throw std::runtime_error("unknown member ordinal " +
                               std::to_string(*member_ordinal) +
                               " for node id: " + node_id);
    }
    auto const port =
      sample_graph_input_port_for(node, *member_ordinal, input_ordinal);
    timeline.set_live_input_value(node_id, *member_ordinal, input_ordinal,
                                  value);
    timeline.set_graph_input_sample_value(port, value);
    lane_views.mark_lane_set_changed();
    return;
  }
  auto const &node = graph_index->logical_nodes[it->second];
  auto const port =
      sample_graph_input_port_for(node, std::nullopt, input_ordinal);
  timeline.set_live_input_value(node_id, input_ordinal, value);
  timeline.set_graph_input_sample_value(port, value);
  lane_views.mark_lane_set_changed();
}

void RuntimeProjectService::clear_sample_input_value_override(
    std::string const &node_id, size_t member_ordinal, size_t input_ordinal) {
  std::scoped_lock lock(mutex);
  rethrow_if_failed();
  if (!graph_index.has_value()) {
    throw std::runtime_error("runtime project service is not initialized");
  }
  auto const it = graph_index->logical_node_index_by_id.find(node_id);
  if (it == graph_index->logical_node_index_by_id.end()) {
    throw std::runtime_error("unknown node id: " + node_id);
  }
  auto const &node = graph_index->logical_nodes[it->second];
  if (member_ordinal >= node.members.size()) {
    throw std::runtime_error("unknown member ordinal " +
                             std::to_string(member_ordinal) +
                             " for node id: " + node_id);
  }
  auto const port =
      sample_graph_input_port_for(node, member_ordinal, input_ordinal);
  timeline.clear_live_input_value_override(node_id, member_ordinal,
                                           input_ordinal);
  timeline.restore_graph_input_sample_inheritance(port);
  lane_views.mark_lane_set_changed();
}

void RuntimeProjectService::request_shutdown() {
  std::scoped_lock lock(mutex);
  shutdown_requested = true;
  if (executor_state) {
    executor_state->request_shutdown();
  }
}
} // namespace iv
